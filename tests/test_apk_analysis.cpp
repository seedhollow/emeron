#include "TestHarness.h"

#include <climits>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "collect/ApkAnalysis.h"
#include "core/Hash.h"

using namespace em;

namespace {

std::string readFixture(const char* name) {
    std::ifstream in{std::string{EMERON_TEST_DATA_DIR} + "/" + name, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// Expected values below come from `openssl x509 -inform DER -noout ...` on the
// same files (see tests/data/make_synthetic_apk.py for how they were made).
constexpr const char* kDebugSha256 =
    "e6:cc:79:f9:c1:c4:2c:35:ce:48:8e:6e:dc:3c:8b:e8:59:a5:ac:34:91:68:14:96:71:86:76:6a:72:e8:5a:8e";
constexpr const char* kDebugSha1 = "8c:6d:5a:26:37:6a:71:a0:d8:a7:1d:67:86:15:25:d0:8c:a2:73:32";

const ZipEntry* findEntry(const std::vector<ZipEntry>& entries, std::string_view name) {
    for (const auto& e : entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------

TEST_CASE("sha1 and sha256 match the FIPS 180 test vectors") {
    CHECK_EQ(toHex(sha1("abc")), std::string{"a9993e364706816aba3e25717850c26c9cd0d89d"});
    CHECK_EQ(toHex(sha256("abc")),
             std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
    CHECK_EQ(toHex(sha256("")),
             std::string{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"});
    // Two blocks: the padding has to spill into a second 64-byte chunk.
    CHECK_EQ(toHex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             std::string{"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"});
    CHECK_EQ(toHex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             std::string{"84983e441c3bd26ebaae4aa1f95129e5e54670f1"});
}

TEST_CASE("base64Decode skips line breaks and stops at padding") {
    CHECK_EQ(base64Decode("UEsF\r\nBg==").value_or("?"), std::string{"PK\x05\x06"});
    CHECK_EQ(base64Decode("").value_or("?"), std::string{});
    CHECK(!base64Decode("not*base64").has_value());
}

TEST_CASE("toHex with a separator") {
    CHECK_EQ(toHex(std::string_view{"\x01\xab", 2}, ':'), std::string{"01:ab"});
}

// ---------------------------------------------------------------------------
// Certificates
// ---------------------------------------------------------------------------

TEST_CASE("parseCertificate reads an RSA debug certificate") {
    const CertificateInfo c = parseCertificate(readFixture("cert_android_debug.der"));
    CHECK(c.error.empty());
    CHECK_EQ(c.subject, std::string{"CN=Android Debug, O=Android, C=US"});
    CHECK_EQ(c.issuer, c.subject);
    CHECK_EQ(c.serialNumber, std::string{"1a2b3c"});
    CHECK_EQ(c.validFrom, std::string{"2026-10-05 01:22:40 UTC"});   // UTCTime
    CHECK_EQ(c.validUntil, std::string{"2056-09-27 01:22:40 UTC"});  // GeneralizedTime (>= 2050)
    CHECK_EQ(c.signatureAlgorithm, std::string{"SHA256 with RSA"});
    CHECK_EQ(c.publicKey, std::string{"RSA 2048-bit"});
    CHECK_EQ(c.sha256, std::string{kDebugSha256});
    CHECK_EQ(c.sha1, std::string{kDebugSha1});
    CHECK(c.isDebugCertificate());
}

TEST_CASE("parseCertificate reads an EC certificate") {
    const CertificateInfo c = parseCertificate(readFixture("cert_ec_release.der"));
    CHECK(c.error.empty());
    CHECK_EQ(c.subject, std::string{"CN=Example Release, OU=Mobile, O=Example Studio, C=ID"});
    CHECK_EQ(c.serialNumber, std::string{"07"});
    CHECK_EQ(c.signatureAlgorithm, std::string{"SHA256 with ECDSA"});
    CHECK_EQ(c.publicKey, std::string{"EC P-256"});
    CHECK_EQ(c.sha256, std::string{"38:7f:dc:cd:0b:42:a5:d4:8a:6c:9b:9e:8e:fb:a4:6d:85:a7:a1:c2:80:"
                                   "1d:eb:8c:9a:83:34:1a:68:4b:fe:aa"});
    CHECK(!c.isDebugCertificate());
}

TEST_CASE("parseCertificate reports garbage instead of crashing") {
    CHECK(!parseCertificate("").error.empty());
    CHECK(!parseCertificate(std::string_view{"\x30\x82\xff\xff\x00", 5}).error.empty());
}

TEST_CASE("certificatesFromPkcs7 finds the certificate in a v1 signature file") {
    const auto certs = certificatesFromPkcs7(readFixture("v1_signature.rsa"));
    CHECK_EQ(certs.size(), std::size_t{1});
    if (!certs.empty()) CHECK_EQ(parseCertificate(certs[0]).sha256, std::string{kDebugSha256});
    CHECK(certificatesFromPkcs7("junk").empty());
}

// ---------------------------------------------------------------------------
// ZIP + signing block, on a real file laid out like an APK
// ---------------------------------------------------------------------------

TEST_CASE("findZipDirectory and parseCentralDirectory read the synthetic APK") {
    const std::string apk = readFixture("synthetic.apk");
    REQUIRE(apk.size() > 1000);

    // Only the tail is handed over, as AppInspector does on the device.
    const std::size_t tailSize = std::min<std::size_t>(apk.size(), 2000);
    const auto dir = findZipDirectory(std::string_view{apk}.substr(apk.size() - tailSize),
                                      apk.size() - tailSize);
    REQUIRE(dir.has_value());
    CHECK_EQ(dir->entryCount, std::uint64_t{6});

    const auto entries = parseCentralDirectory(std::string_view{apk}.substr(
        dir->centralDirectoryOffset, dir->centralDirectorySize));
    CHECK_EQ(entries.size(), std::size_t{6});

    const ZipEntry* good = findEntry(entries, "lib/arm64-v8a/libgood.so");
    const ZipEntry* bad = findEntry(entries, "lib/arm64-v8a/libbad.so");
    const ZipEntry* old = findEntry(entries, "lib/armeabi-v7a/libold.so");
    REQUIRE(good != nullptr);
    REQUIRE(bad != nullptr);
    REQUIRE(old != nullptr);
    CHECK_EQ(good->method, std::uint16_t{0});
    CHECK_EQ(old->method, std::uint16_t{8});
    CHECK_EQ(good->uncompressedSize, std::uint64_t{8192});

    const auto goodData = zipDataOffset(std::string_view{apk}.substr(good->localHeaderOffset, 30),
                                        good->localHeaderOffset);
    const auto badData = zipDataOffset(std::string_view{apk}.substr(bad->localHeaderOffset, 30),
                                       bad->localHeaderOffset);
    REQUIRE(goodData.has_value());
    REQUIRE(badData.has_value());
    CHECK_EQ(*goodData % 16384, std::uint64_t{0});
    CHECK(*badData % 16384 != 0);

    const ElfInfo goodElf = parseElfHeader(std::string_view{apk}.substr(*goodData, 4096));
    CHECK(goodElf.valid);
    CHECK_EQ(goodElf.bits, 64);
    CHECK_EQ(goodElf.machine, std::string{"arm64"});
    CHECK_EQ(goodElf.loadAlignment, std::uint64_t{16384});  // PT_DYNAMIC's align 8 is ignored
    CHECK_EQ(parseElfHeader(std::string_view{apk}.substr(*badData, 4096)).loadAlignment,
             std::uint64_t{4096});
}

TEST_CASE("parseSigningBlock reads the v2 and v3 signers") {
    const std::string apk = readFixture("synthetic.apk");
    const auto dir = findZipDirectory(apk, 0);
    REQUIRE(dir.has_value());
    const auto offset = static_cast<std::size_t>(dir->centralDirectoryOffset);

    const SigningBlock block = parseSigningBlock(std::string_view{apk}.substr(0, offset));
    CHECK(block.found);
    CHECK(block.error.empty());
    CHECK_EQ(block.signers.size(), std::size_t{2});
    if (block.signers.size() == 2) {
        CHECK_EQ(block.signers[0].scheme, 2);
        CHECK_EQ(block.signers[1].scheme, 3);
        CHECK_EQ(block.signers[1].minSdk, 24);
        CHECK_EQ(block.signers[1].maxSdk, INT_MAX);
        REQUIRE(!block.signers[1].certificates.empty());
        CHECK_EQ(block.signers[1].certificates[0].sha256, std::string{kDebugSha256});
    }
    CHECK_EQ(block.otherBlocks.size(), std::size_t{1});
    if (!block.otherBlocks.empty()) {
        CHECK(block.otherBlocks[0].find("Verity padding") != std::string::npos);
    }
}

TEST_CASE("parseSigningBlock asks for more bytes when the first read was short") {
    const std::string apk = readFixture("synthetic.apk");
    const auto dir = findZipDirectory(apk, 0);
    REQUIRE(dir.has_value());
    const auto offset = static_cast<std::size_t>(dir->centralDirectoryOffset);

    const SigningBlock partial = parseSigningBlock(std::string_view{apk}.substr(offset - 64, 64));
    CHECK(partial.found);
    CHECK(partial.signers.empty());
    CHECK(partial.bytesNeeded > 64);
    REQUIRE(partial.bytesNeeded <= offset);

    const SigningBlock whole = parseSigningBlock(
        std::string_view{apk}.substr(offset - partial.bytesNeeded, partial.bytesNeeded));
    CHECK_EQ(whole.signers.size(), std::size_t{2});
}

TEST_CASE("parseSigningBlock reports no block for a v1-only APK") {
    const SigningBlock block = parseSigningBlock(std::string(200, 'x'));
    CHECK(!block.found);
    CHECK(block.signers.empty());
}

TEST_CASE("parseElfHeader reads a 32-bit ARM library") {
    std::string elf(4096, '\0');
    const char header[] = "\x7f" "ELF\x01\x01\x01";
    std::memcpy(elf.data(), header, 7);
    const auto put16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(&elf[at], &v, 2); };
    const auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(&elf[at], &v, 4); };
    put16(18, 0x28);  // EM_ARM
    put32(28, 52);    // e_phoff
    put16(42, 32);    // e_phentsize
    put16(44, 1);     // e_phnum
    put32(52, 1);     // PT_LOAD
    put32(52 + 28, 0x1000);
    const ElfInfo info = parseElfHeader(elf);
    CHECK(info.valid);
    CHECK_EQ(info.bits, 32);
    CHECK_EQ(info.machine, std::string{"arm"});
    CHECK_EQ(info.loadAlignment, std::uint64_t{4096});
}

TEST_CASE("parseElfHeader rejects what is not ELF, and headers past the bytes read") {
    CHECK(!parseElfHeader("PK\x03\x04 not elf at all, just some text padding it out a bit....").valid);
    std::string elf(64, '\0');
    std::memcpy(elf.data(), "\x7f" "ELF\x02\x01\x01", 7);
    const std::uint64_t phoff = 100000;
    std::memcpy(&elf[32], &phoff, 8);
    const std::uint16_t one = 1;
    const std::uint16_t size = 56;
    std::memcpy(&elf[54], &size, 2);
    std::memcpy(&elf[56], &one, 2);
    const ElfInfo info = parseElfHeader(elf);
    CHECK(!info.valid);
    CHECK(!info.error.empty());
}
