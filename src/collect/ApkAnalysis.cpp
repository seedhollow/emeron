#include "collect/ApkAnalysis.h"

#include <algorithm>
#include <limits>

#include "core/Hash.h"
#include "core/StringUtil.h"

namespace em {
namespace {

// --- little-endian readers ----------------------------------------------------

std::uint64_t le(std::string_view b, std::size_t at, std::size_t width) noexcept {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b[at + i])) << (8 * i);
    }
    return v;
}
std::uint16_t le16(std::string_view b, std::size_t at) noexcept {
    return static_cast<std::uint16_t>(le(b, at, 2));
}
std::uint32_t le32(std::string_view b, std::size_t at) noexcept {
    return static_cast<std::uint32_t>(le(b, at, 4));
}
std::uint64_t le64(std::string_view b, std::size_t at) noexcept { return le(b, at, 8); }

// A cursor over length-prefixed (u32 LE) APK Signing Block structures.
struct Reader {
    std::string_view data;
    bool ok = true;

    [[nodiscard]] bool empty() const noexcept { return data.empty(); }
    std::uint32_t u32() {
        if (data.size() < 4) {
            ok = false;
            return 0;
        }
        const std::uint32_t v = le32(data, 0);
        data.remove_prefix(4);
        return v;
    }
    std::string_view prefixed() {
        const std::uint32_t n = u32();
        if (!ok || data.size() < n) {
            ok = false;
            return {};
        }
        const std::string_view out = data.substr(0, n);
        data.remove_prefix(n);
        return out;
    }
};

// --- DER ------------------------------------------------------------------------

struct Der {
    std::uint8_t tag = 0;
    std::string_view content;
    std::string_view whole;  // tag + length + content
};

std::optional<Der> readDer(std::string_view& in) {
    if (in.size() < 2) return std::nullopt;
    const std::string_view start = in;
    Der d;
    d.tag = static_cast<std::uint8_t>(in[0]);
    std::size_t pos = 1;
    std::size_t length = static_cast<std::uint8_t>(in[pos++]);
    if (length & 0x80) {
        const std::size_t bytes = length & 0x7F;
        if (bytes == 0 || bytes > 4 || in.size() < pos + bytes) return std::nullopt;
        length = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            length = (length << 8) | static_cast<std::uint8_t>(in[pos++]);
        }
    }
    if (in.size() - pos < length) return std::nullopt;
    d.content = in.substr(pos, length);
    d.whole = start.substr(0, pos + length);
    in.remove_prefix(pos + length);
    return d;
}

constexpr std::uint8_t kSequence = 0x30;
constexpr std::uint8_t kInteger = 0x02;
constexpr std::uint8_t kBitString = 0x03;
constexpr std::uint8_t kOid = 0x06;

std::string decodeOid(std::string_view bytes) {
    if (bytes.empty()) return {};
    const auto first = static_cast<std::uint8_t>(bytes[0]);
    std::string out = std::to_string(first / 40) + "." + std::to_string(first % 40);
    std::uint64_t value = 0;
    for (std::size_t i = 1; i < bytes.size(); ++i) {
        const auto b = static_cast<std::uint8_t>(bytes[i]);
        value = (value << 7) | (b & 0x7F);
        if ((b & 0x80) == 0) {
            out += "." + std::to_string(value);
            value = 0;
        }
    }
    return out;
}

const char* attributeName(const std::string& oid) {
    if (oid == "2.5.4.3") return "CN";
    if (oid == "2.5.4.6") return "C";
    if (oid == "2.5.4.7") return "L";
    if (oid == "2.5.4.8") return "ST";
    if (oid == "2.5.4.10") return "O";
    if (oid == "2.5.4.11") return "OU";
    if (oid == "2.5.4.5") return "SERIALNUMBER";
    if (oid == "1.2.840.113549.1.9.1") return "EMAIL";
    return nullptr;
}

std::string algorithmName(const std::string& oid) {
    if (oid == "1.2.840.113549.1.1.11") return "SHA256 with RSA";
    if (oid == "1.2.840.113549.1.1.12") return "SHA384 with RSA";
    if (oid == "1.2.840.113549.1.1.13") return "SHA512 with RSA";
    if (oid == "1.2.840.113549.1.1.5") return "SHA1 with RSA";
    if (oid == "1.2.840.113549.1.1.4") return "MD5 with RSA";
    if (oid == "1.2.840.113549.1.1.10") return "RSA-PSS";
    if (oid == "1.2.840.10045.4.3.2") return "SHA256 with ECDSA";
    if (oid == "1.2.840.10045.4.3.3") return "SHA384 with ECDSA";
    if (oid == "1.2.840.10045.4.3.4") return "SHA512 with ECDSA";
    if (oid == "1.2.840.10045.4.1") return "SHA1 with ECDSA";
    if (oid == "1.2.840.10040.4.3") return "SHA1 with DSA";
    if (oid == "2.16.840.1.101.3.4.3.2") return "SHA256 with DSA";
    return oid;
}

std::string curveName(const std::string& oid) {
    if (oid == "1.2.840.10045.3.1.7") return "P-256";
    if (oid == "1.3.132.0.34") return "P-384";
    if (oid == "1.3.132.0.35") return "P-521";
    return oid;
}

// RFC 4514 order -- most specific first ("CN=..., O=..., C=US"), as apksigner,
// keytool and openssl print it. DER stores the reverse.
std::string formatName(std::string_view name) {
    std::vector<std::string> parts;
    std::string_view rdns = name;
    while (auto set = readDer(rdns)) {
        std::string_view attrs = set->content;
        while (auto attr = readDer(attrs)) {
            std::string_view pair = attr->content;
            const auto oid = readDer(pair);
            const auto value = readDer(pair);
            if (!oid || !value) continue;
            const std::string dotted = decodeOid(oid->content);
            const char* label = attributeName(dotted);
            // Printable/UTF8/IA5 strings all read as bytes.
            parts.push_back(std::string{label != nullptr ? label : dotted} + "=" +
                            std::string{value->content});
        }
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty()) out += ", ";
        out += *it;
    }
    return out;
}

std::string formatTime(const Der& t) {
    const std::string_view s = t.content;
    std::string year;
    std::string_view rest;
    if (t.tag == 0x17 && s.size() >= 12) {  // UTCTime YYMMDDHHMMSSZ
        const int yy = parseNumber<int>(s.substr(0, 2)).value_or(0);
        year = std::to_string(yy >= 50 ? 1900 + yy : 2000 + yy);
        rest = s.substr(2);
    } else if (t.tag == 0x18 && s.size() >= 14) {  // GeneralizedTime YYYYMMDDHHMMSSZ
        year = std::string{s.substr(0, 4)};
        rest = s.substr(4);
    } else {
        return std::string{s};
    }
    return year + "-" + std::string{rest.substr(0, 2)} + "-" + std::string{rest.substr(2, 2)} +
           " " + std::string{rest.substr(4, 2)} + ":" + std::string{rest.substr(6, 2)} + ":" +
           std::string{rest.substr(8, 2)} + " UTC";
}

// Bits in a big-endian unsigned INTEGER, ignoring DER's sign-padding zeros.
int integerBits(std::string_view bytes) {
    while (!bytes.empty() && bytes.front() == '\0') bytes.remove_prefix(1);
    if (bytes.empty()) return 0;
    int bits = static_cast<int>(bytes.size()) * 8;
    for (auto top = static_cast<std::uint8_t>(bytes.front()); (top & 0x80) == 0; top <<= 1) --bits;
    return bits;
}

std::string describePublicKey(std::string_view spki) {
    std::string_view in = spki;
    const auto algorithm = readDer(in);
    const auto key = readDer(in);
    if (!algorithm || !key) return "unknown";
    std::string_view alg = algorithm->content;
    const auto oid = readDer(alg);
    if (!oid) return "unknown";
    const std::string dotted = decodeOid(oid->content);
    const auto params = readDer(alg);

    if (dotted == "1.2.840.113549.1.1.1" && key->tag == kBitString && key->content.size() > 1) {
        std::string_view bits = key->content.substr(1);  // skip the unused-bits byte
        const auto rsa = readDer(bits);
        if (rsa) {
            std::string_view fields = rsa->content;
            if (const auto modulus = readDer(fields)) {
                return "RSA " + std::to_string(integerBits(modulus->content)) + "-bit";
            }
        }
        return "RSA";
    }
    if (dotted == "1.2.840.10045.2.1") {
        return "EC " + (params && params->tag == kOid ? curveName(decodeOid(params->content))
                                                     : std::string{"?"});
    }
    if (dotted == "1.2.840.10040.4.1") {
        if (params && params->tag == kSequence) {
            std::string_view pqg = params->content;
            if (const auto p = readDer(pqg)) return "DSA " + std::to_string(integerBits(p->content)) + "-bit";
        }
        return "DSA";
    }
    return dotted;
}

std::string blockName(std::uint32_t id) {
    switch (id) {
        case 0x7109871a: return "v2 signature";
        case 0xf05368c0: return "v3 signature";
        case 0x1b93ad61: return "v3.1 signature";
        case 0x42726577: return "Verity padding (alignment for fs-verity)";
        case 0x6dff800d: return "Source stamp v2 (which store or tool built it)";
        case 0x2b09189e: return "Source stamp v1";
        case 0x2146444e: return "Google Play metadata";
        case 0x504b4453: return "Dependency info (Android Gradle Plugin, encrypted)";
        default: return format("unknown block 0x%08x", static_cast<unsigned>(id));
    }
}

std::vector<CertificateInfo> parseSignerCertificates(std::string_view certificates) {
    std::vector<CertificateInfo> out;
    Reader certs{certificates};
    while (!certs.empty() && certs.ok) {
        const std::string_view der = certs.prefixed();
        if (!certs.ok) break;
        out.push_back(parseCertificate(der));
    }
    return out;
}

ApkSigner parseSigner(std::string_view signer, int scheme) {
    ApkSigner out;
    out.scheme = scheme;
    Reader r{signer};
    Reader signedData{r.prefixed()};
    signedData.prefixed();  // digests
    out.certificates = parseSignerCertificates(signedData.prefixed());
    if (scheme != 2) {
        out.minSdk = static_cast<int>(signedData.u32());
        out.maxSdk = static_cast<int>(signedData.u32());
        // INT_MAX is how v3 spells "no upper bound".
        if (out.maxSdk < 0) out.maxSdk = std::numeric_limits<int>::max();
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// ZIP
// ---------------------------------------------------------------------------

std::optional<ZipDirectory> findZipDirectory(std::string_view tail, std::uint64_t tailOffset) {
    constexpr std::size_t kEocdSize = 22;
    if (tail.size() < kEocdSize) return std::nullopt;
    // The record ends with a variable-length comment, so search backwards for
    // a signature whose comment length lands exactly on the end of the file.
    for (std::size_t at = tail.size() - kEocdSize + 1; at-- > 0;) {
        if (le32(tail, at) != 0x06054b50) continue;
        if (at + kEocdSize + le16(tail, at + 20) != tail.size()) continue;

        ZipDirectory dir;
        dir.entryCount = le16(tail, at + 10);
        dir.centralDirectorySize = le32(tail, at + 12);
        dir.centralDirectoryOffset = le32(tail, at + 16);

        const bool zip64 = dir.centralDirectoryOffset == 0xFFFFFFFF ||
                           dir.centralDirectorySize == 0xFFFFFFFF || dir.entryCount == 0xFFFF;
        if (zip64) {
            // ZIP64 locator: 20 bytes just before the EOCD.
            if (at < 20 || le32(tail, at - 20) != 0x07064b50) return std::nullopt;
            const std::uint64_t recordOffset = le64(tail, at - 20 + 8);
            if (recordOffset < tailOffset) return std::nullopt;
            const auto rec = static_cast<std::size_t>(recordOffset - tailOffset);
            if (rec + 56 > tail.size() || le32(tail, rec) != 0x06064b50) return std::nullopt;
            dir.entryCount = le64(tail, rec + 32);
            dir.centralDirectorySize = le64(tail, rec + 40);
            dir.centralDirectoryOffset = le64(tail, rec + 48);
        }
        return dir;
    }
    return std::nullopt;
}

std::vector<ZipEntry> parseCentralDirectory(std::string_view directory) {
    std::vector<ZipEntry> entries;
    std::size_t at = 0;
    while (at + 46 <= directory.size() && le32(directory, at) == 0x02014b50) {
        ZipEntry e;
        e.method = le16(directory, at + 10);
        e.compressedSize = le32(directory, at + 20);
        e.uncompressedSize = le32(directory, at + 24);
        const std::size_t nameLength = le16(directory, at + 28);
        const std::size_t extraLength = le16(directory, at + 30);
        const std::size_t commentLength = le16(directory, at + 32);
        e.localHeaderOffset = le32(directory, at + 42);
        if (at + 46 + nameLength + extraLength + commentLength > directory.size()) break;
        e.name = std::string{directory.substr(at + 46, nameLength)};

        // ZIP64 extended information: only the fields that overflowed are
        // present, in this order.
        std::string_view extra = directory.substr(at + 46 + nameLength, extraLength);
        while (extra.size() >= 4) {
            const std::uint16_t id = le16(extra, 0);
            const std::uint16_t size = le16(extra, 2);
            if (extra.size() < 4U + size) break;
            if (id == 0x0001) {
                std::size_t p = 4;
                const auto take = [&](std::uint64_t& field) {
                    if (field == 0xFFFFFFFF && p + 8 <= 4U + size) {
                        field = le64(extra, p);
                        p += 8;
                    }
                };
                take(e.uncompressedSize);
                take(e.compressedSize);
                take(e.localHeaderOffset);
            }
            extra.remove_prefix(4U + size);
        }
        entries.push_back(std::move(e));
        at += 46 + nameLength + extraLength + commentLength;
    }
    return entries;
}

std::optional<std::uint64_t> zipDataOffset(std::string_view localHeader,
                                           std::uint64_t localHeaderOffset) {
    if (localHeader.size() < 30 || le32(localHeader, 0) != 0x04034b50) return std::nullopt;
    return localHeaderOffset + 30 + le16(localHeader, 26) + le16(localHeader, 28);
}

// ---------------------------------------------------------------------------
// Certificates
// ---------------------------------------------------------------------------

bool CertificateInfo::isDebugCertificate() const {
    return subject.find("CN=Android Debug") != std::string::npos;
}

CertificateInfo parseCertificate(std::string_view der) {
    CertificateInfo info;
    info.sha256 = toHex(sha256(der), ':');
    info.sha1 = toHex(sha1(der), ':');

    std::string_view in = der;
    const auto cert = readDer(in);
    if (!cert || cert->tag != kSequence) {
        info.error = "not a DER certificate";
        return info;
    }
    std::string_view certFields = cert->content;
    const auto tbs = readDer(certFields);
    const auto sigAlg = readDer(certFields);
    if (!tbs || !sigAlg) {
        info.error = "truncated certificate";
        return info;
    }
    {
        std::string_view alg = sigAlg->content;
        if (const auto oid = readDer(alg)) info.signatureAlgorithm = algorithmName(decodeOid(oid->content));
    }

    std::string_view t = tbs->content;
    auto field = readDer(t);
    if (field && field->tag == 0xA0) field = readDer(t);  // explicit version
    if (field && field->tag == kInteger) {
        std::string_view serial = field->content;
        while (serial.size() > 1 && serial.front() == '\0') serial.remove_prefix(1);
        info.serialNumber = toHex(serial);
    }
    readDer(t);  // signature algorithm, repeated
    if (const auto issuer = readDer(t)) info.issuer = formatName(issuer->content);
    if (const auto validity = readDer(t)) {
        std::string_view v = validity->content;
        if (const auto from = readDer(v)) info.validFrom = formatTime(*from);
        if (const auto until = readDer(v)) info.validUntil = formatTime(*until);
    }
    if (const auto subject = readDer(t)) info.subject = formatName(subject->content);
    if (const auto spki = readDer(t)) info.publicKey = describePublicKey(spki->content);
    return info;
}

std::vector<std::string> certificatesFromPkcs7(std::string_view der) {
    std::vector<std::string> out;
    std::string_view in = der;
    const auto contentInfo = readDer(in);
    if (!contentInfo || contentInfo->tag != kSequence) return out;
    std::string_view ci = contentInfo->content;
    readDer(ci);  // contentType OID
    const auto explicitContent = readDer(ci);
    if (!explicitContent || explicitContent->tag != 0xA0) return out;
    std::string_view ec = explicitContent->content;
    const auto signedData = readDer(ec);
    if (!signedData || signedData->tag != kSequence) return out;

    std::string_view sd = signedData->content;
    readDer(sd);  // version
    readDer(sd);  // digestAlgorithms
    readDer(sd);  // encapContentInfo
    const auto certs = readDer(sd);
    if (!certs || certs->tag != 0xA0) return out;  // [0] IMPLICIT certificates
    std::string_view list = certs->content;
    while (auto c = readDer(list)) {
        if (c->tag == kSequence) out.emplace_back(c->whole);
    }
    return out;
}

// ---------------------------------------------------------------------------
// APK Signing Block
// ---------------------------------------------------------------------------

SigningBlock parseSigningBlock(std::string_view bytes) {
    SigningBlock block;
    constexpr std::string_view kMagic = "APK Sig Block 42";
    if (bytes.size() < 32 || bytes.substr(bytes.size() - 16) != kMagic) return block;  // none
    block.found = true;

    const std::uint64_t size = le64(bytes, bytes.size() - 24);  // excludes its leading size field
    if (size < 24 || size > (1ULL << 31)) {
        block.error = "corrupt APK Signing Block";
        return block;
    }
    const std::uint64_t total = size + 8;
    if (total > bytes.size()) {
        block.bytesNeeded = total;
        return block;
    }
    std::string_view pairs = bytes.substr(bytes.size() - total + 8, total - 8 - 24);

    while (pairs.size() >= 12) {
        const std::uint64_t length = le64(pairs, 0);
        if (length < 4 || length > pairs.size() - 8) {
            block.error = "corrupt APK Signing Block entry";
            break;
        }
        const std::uint32_t id = le32(pairs, 8);
        const std::string_view value = pairs.substr(12, static_cast<std::size_t>(length - 4));
        pairs.remove_prefix(static_cast<std::size_t>(8 + length));

        const int scheme = id == 0x7109871a ? 2 : id == 0xf05368c0 ? 3 : id == 0x1b93ad61 ? 31 : 0;
        if (scheme == 0) {
            block.otherBlocks.push_back(blockName(id));
            continue;
        }
        Reader r{value};
        Reader signers{r.prefixed()};
        while (!signers.empty() && signers.ok) {
            const std::string_view signer = signers.prefixed();
            if (!signers.ok) break;
            block.signers.push_back(parseSigner(signer, scheme));
        }
        if (!r.ok || !signers.ok) block.error = "could not read the " + blockName(id);
    }
    return block;
}

// ---------------------------------------------------------------------------
// ELF
// ---------------------------------------------------------------------------

ElfInfo parseElfHeader(std::string_view b) {
    ElfInfo info;
    if (b.size() < 52 || b.substr(0, 4) != std::string_view{"\x7f" "ELF", 4}) {
        info.error = "not an ELF file";
        return info;
    }
    const auto elfClass = static_cast<std::uint8_t>(b[4]);
    if (static_cast<std::uint8_t>(b[5]) != 1) {
        info.error = "big-endian ELF";
        return info;
    }
    const bool is64 = elfClass == 2;
    info.bits = is64 ? 64 : 32;

    switch (le16(b, 18)) {
        case 0x28: info.machine = "arm"; break;
        case 0xB7: info.machine = "arm64"; break;
        case 0x03: info.machine = "x86"; break;
        case 0x3E: info.machine = "x86_64"; break;
        case 0xF3: info.machine = is64 ? "riscv64" : "riscv32"; break;
        default: info.machine = format("machine 0x%x", static_cast<unsigned>(le16(b, 18)));
    }

    if (is64 && b.size() < 64) {
        info.error = "truncated ELF header";
        return info;
    }
    const std::uint64_t phoff = is64 ? le64(b, 32) : le32(b, 28);
    const std::size_t phentsize = is64 ? le16(b, 54) : le16(b, 42);
    const std::size_t phnum = is64 ? le16(b, 56) : le16(b, 44);
    if (phentsize == 0 || phnum == 0) {
        info.error = "no program headers";
        return info;
    }
    if (phoff + phentsize * phnum > b.size()) {
        info.error = "program headers are past the bytes read";
        return info;
    }
    std::uint64_t minAlign = 0;
    for (std::size_t i = 0; i < phnum; ++i) {
        const auto at = static_cast<std::size_t>(phoff + i * phentsize);
        if (le32(b, at) != 1) continue;  // PT_LOAD
        const std::uint64_t align = is64 ? le64(b, at + 48) : le32(b, at + 28);
        minAlign = minAlign == 0 ? align : std::min(minAlign, align);
    }
    info.loadAlignment = minAlign;
    info.valid = true;
    return info;
}

}  // namespace em
