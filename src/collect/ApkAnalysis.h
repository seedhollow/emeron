#pragma once

// Reading what is inside an APK without downloading it.
//
// An APK is a ZIP. Everything needed here sits in a few small, known places:
//   * the End Of Central Directory record, in the last 64 KB, which says where
//     the central directory (the ZIP's index) is;
//   * the central directory itself, which names every entry -- including each
//     lib/<abi>/*.so -- with its size, compression and offset;
//   * the APK Signing Block, immediately before the central directory, which
//     holds the v2/v3 signers and their X.509 certificates;
//   * the first few KB of each native library, where its ELF header and
//     program headers are.
// AppInspector fetches exactly those byte ranges with `dd` on the device; this
// file only parses bytes, so all of it is unit-testable.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace em {

// --- ZIP ------------------------------------------------------------------

struct ZipEntry {
    std::string name;
    std::uint16_t method = 0;  // 0 stored, 8 deflated
    std::uint64_t compressedSize = 0;
    std::uint64_t uncompressedSize = 0;
    std::uint64_t localHeaderOffset = 0;
};

struct ZipDirectory {
    std::uint64_t centralDirectoryOffset = 0;
    std::uint64_t centralDirectorySize = 0;
    std::uint64_t entryCount = 0;
};

// `tail` is the last bytes of the file, starting at absolute offset
// `tailOffset`. Handles ZIP64 when its records are inside `tail`.
[[nodiscard]] std::optional<ZipDirectory> findZipDirectory(std::string_view tail,
                                                           std::uint64_t tailOffset);
[[nodiscard]] std::vector<ZipEntry> parseCentralDirectory(std::string_view directory);
// Where an entry's data starts, from the first bytes of its local header.
[[nodiscard]] std::optional<std::uint64_t> zipDataOffset(std::string_view localHeader,
                                                         std::uint64_t localHeaderOffset);

// --- certificates -----------------------------------------------------------

struct CertificateInfo {
    std::string subject;             // "CN=..., O=..., C=..."
    std::string issuer;
    std::string serialNumber;        // hex
    std::string validFrom;           // "2008-08-21 23:13:34 UTC"
    std::string validUntil;
    std::string signatureAlgorithm;  // "SHA256 with RSA"
    std::string publicKey;           // "RSA 2048-bit", "EC P-256"
    std::string sha256;              // fingerprint of the DER bytes, aa:bb:...
    std::string sha1;
    std::string error;

    // Signed with the key Android Studio generates for debug builds.
    [[nodiscard]] bool isDebugCertificate() const;
};

[[nodiscard]] CertificateInfo parseCertificate(std::string_view der);

// The certificates inside a v1 (JAR) signature file, META-INF/*.RSA|DSA|EC,
// which is a PKCS#7 SignedData structure.
[[nodiscard]] std::vector<std::string> certificatesFromPkcs7(std::string_view der);

// --- APK Signing Block --------------------------------------------------------

struct ApkSigner {
    int scheme = 0;  // 2, 3 or 31 (v3.1)
    std::vector<CertificateInfo> certificates;  // first is the signing certificate
    int minSdk = -1;  // v3 only
    int maxSdk = -1;
};

struct SigningBlock {
    bool found = false;
    std::vector<ApkSigner> signers;
    std::vector<std::string> otherBlocks;  // known names, or the id in hex
    // Set when `bytes` did not reach back to the start of the block; fetch
    // this many bytes before the central directory and parse again.
    std::uint64_t bytesNeeded = 0;
    std::string error;
};

// `bytes` must END exactly at the central directory offset.
[[nodiscard]] SigningBlock parseSigningBlock(std::string_view bytes);

// --- ELF ------------------------------------------------------------------------

struct ElfInfo {
    bool valid = false;
    int bits = 0;              // 32 or 64
    std::string machine;       // "arm64", "arm", "x86_64", "x86", "riscv64"
    std::uint64_t loadAlignment = 0;  // smallest p_align of the PT_LOAD segments
    std::string error;
};

// The first few KB of a .so: the ELF header plus the program headers.
[[nodiscard]] ElfInfo parseElfHeader(std::string_view bytes);

}  // namespace em
