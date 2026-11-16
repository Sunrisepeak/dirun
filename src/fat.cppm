// A FAT32 disk written from a directory tree (0.6.0). The module depends on nothing of dirun's and
// only writes: an emulator is given the disk, and nothing is read back from it.
//
// THE DISK. A master boot record with one partition, type 0Ch (FAT32, LBA addressed), from 1 MiB
// to the end; in it a FAT32 volume of 512-byte sectors, one sector a cluster, two FATs, 32 reserved
// sectors (the boot sector at 0, FSInfo at 1, their copies at 6 and 7) and the root directory at
// cluster 2. A volume is FAT32 from 65525 clusters (Microsoft's FAT specification, "FAT type
// determination"), so the smallest disk is about 34 MiB; it is written sparse where the host's file
// system allows.
//
// THE TREE. Every regular file and directory below the root, in name order, each stored in
// consecutive clusters. A name that is a valid upper-case 8.3 name is stored as such; any other
// has long-name entries (UTF-16, at most 255 units) before an 8.3 alias: the name in upper case
// where that is a valid 8.3 name, else a generated one (`NAME~1.EXT').
// Timestamps are the FAT epoch, 1980-01-01 00:00: the disk records no time of this machine.
export module dirun.fat;

import std;

export namespace dirun::fat {

struct options {
    std::uint64_t free_bytes = 32ull << 20;  // room for what the guest writes, beyond the tree
    std::string label = "DIRUN";             // the volume's label: at most 11 characters
};

// Writes `image` holding the files and directories below `root`; the error's text otherwise.
std::expected<void, std::string> write(const std::filesystem::path& root, const std::filesystem::path& image,
                                       const options& opts = {});

}  // namespace dirun::fat

namespace dirun::fat {
namespace {

constexpr std::uint32_t kSector = 512;
constexpr std::uint32_t kPartitionStart = 2048;   // sectors: 1 MiB
constexpr std::uint32_t kReserved = 32;
constexpr std::uint32_t kMinClusters = 65525 + 16;
constexpr std::uint32_t kEnd = 0x0FFFFFFF;
constexpr std::uint16_t kEpoch = (1 << 5) | 1;    // 1980-01-01

struct node {
    std::filesystem::path host;
    std::u16string name;
    bool dir = false;
    std::uint64_t size = 0;
    std::vector<node> children;
    std::array<char, 11> short_name{};
    bool long_name = false;
    std::uint32_t first = 0;     // the first cluster; 0 for an empty file
    std::uint32_t clusters = 0;
};

std::expected<std::u16string, std::string> utf16(const std::u8string& s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        const int n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size())
            return std::unexpected(std::string("a name that is not UTF-8"));
        char32_t c = n == 1 ? b : n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
        for (int k = 1; k < n; ++k) {
            const auto t = static_cast<unsigned char>(s[i + k]);
            if ((t >> 6) != 2)
                return std::unexpected(std::string("a name that is not UTF-8"));
            c = (c << 6) | (t & 0x3F);
        }
        i += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(c));
        }
    }
    return out;
}

bool short_char(char16_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           std::u16string_view(u"!#$%&'()-@^_`{}~").find(c) != std::u16string_view::npos;
}

// The name's 8.3 form when it is exactly a valid upper-case 8.3 name.
std::optional<std::array<char, 11>> as_short(const std::u16string& name) {
    const auto dot = name.find(u'.');
    const auto base = name.substr(0, dot);
    const auto ext = dot == std::u16string::npos ? std::u16string() : name.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3 || (dot != std::u16string::npos && ext.empty()))
        return std::nullopt;
    std::array<char, 11> out;
    out.fill(' ');
    for (std::size_t i = 0; i < base.size(); ++i) {
        if (!short_char(base[i])) return std::nullopt;
        out[i] = static_cast<char>(base[i]);
    }
    for (std::size_t i = 0; i < ext.size(); ++i) {
        if (!short_char(ext[i])) return std::nullopt;
        out[8 + i] = static_cast<char>(ext[i]);
    }
    return out;
}

// The basis of a generated alias: upper case, the characters 8.3 lacks as `_', blanks and the dots
// before the last removed (Microsoft's FAT specification, "The Basis-Name Generation Algorithm").
std::pair<std::string, std::string> basis(const std::u16string& name) {
    const auto last = name.rfind(u'.');
    auto take = [](std::u16string_view part, std::size_t max) {
        std::string out;
        for (char16_t c : part) {
            if (c == u' ' || c == u'.') continue;
            if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - u'a' + u'A');
            out.push_back(short_char(c) ? static_cast<char>(c) : '_');
            if (out.size() == max) break;
        }
        return out;
    };
    const bool has_ext = last != std::u16string::npos && last != 0;
    std::string base = take(std::u16string_view(name).substr(0, has_ext ? last : name.size()), 8);
    std::string ext = has_ext ? take(std::u16string_view(name).substr(last + 1), 3) : std::string();
    if (base.empty()) base = "_";
    return {base, ext};
}

std::array<char, 11> packed(const std::string& base, const std::string& ext) {
    std::array<char, 11> out;
    out.fill(' ');
    std::ranges::copy(base, out.begin());
    std::ranges::copy(ext, out.begin() + 8);
    return out;
}

void name_children(node& dir) {
    std::set<std::array<char, 11>> taken;
    for (auto& c : dir.children)
        if (auto s = as_short(c.name); s && taken.insert(*s).second)
            c.short_name = *s;
        else
            c.long_name = true;
    for (auto& c : dir.children) {
        if (!c.long_name) continue;
        // A name that differs from an 8.3 name only in case keeps that name as its alias.
        std::u16string upper = c.name;
        for (auto& u : upper)
            if (u >= u'a' && u <= u'z') u = static_cast<char16_t>(u - u'a' + u'A');
        if (auto s = as_short(upper); s && taken.insert(*s).second) {
            c.short_name = *s;
            continue;
        }
        const auto [base, ext] = basis(c.name);
        for (unsigned n = 1;; ++n) {
            const auto tail = std::format("~{}", n);
            const auto alias = packed(base.substr(0, 8 - tail.size()) + tail, ext);
            if (taken.insert(alias).second) {
                c.short_name = alias;
                break;
            }
        }
    }
    for (auto& c : dir.children)
        if (c.dir) name_children(c);
}

std::expected<void, std::string> read_tree(node& dir) {
    std::vector<std::filesystem::directory_entry> entries;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir.host, ec))
        entries.push_back(e);
    if (ec)
        return std::unexpected("cannot list " + dir.host.string() + ": " + ec.message());
    std::ranges::sort(entries, {}, [](const auto& e) { return e.path().filename(); });
    for (const auto& e : entries) {
        node n;
        n.host = e.path();
        auto name = utf16(e.path().filename().u8string());
        if (!name)
            return std::unexpected(name.error() + ": " + e.path().string());
        if (name->size() > 255)
            return std::unexpected("a name longer than FAT's 255 characters: " + e.path().string());
        n.name = std::move(*name);
        if (e.is_directory()) {
            n.dir = true;
            if (auto r = read_tree(n); !r) return r;
        } else if (e.is_regular_file()) {
            n.size = e.file_size();
            if (n.size > 0xFFFFFFFFull)
                return std::unexpected("a file larger than FAT's 4 GiB: " + e.path().string());
        } else {
            continue;
        }
        dir.children.push_back(std::move(n));
    }
    return {};
}

std::uint32_t long_entries(const node& n) {
    return n.long_name ? static_cast<std::uint32_t>((n.name.size() + 12) / 13) : 0;
}

std::uint64_t entries_of(const node& dir, bool root) {
    std::uint64_t n = root ? 1 : 2;   // the volume label; `.' and `..'
    for (const auto& c : dir.children)
        n += 1 + long_entries(c);
    return n;
}

void allocate(node& dir, bool root, std::uint32_t& next) {
    dir.first = next;
    dir.clusters = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, (entries_of(dir, root) * 32 + kSector - 1) / kSector));
    next += dir.clusters;
    for (auto& c : dir.children)
        if (c.dir) {
            allocate(c, false, next);
        } else if (c.size > 0) {
            c.first = next;
            c.clusters = static_cast<std::uint32_t>((c.size + kSector - 1) / kSector);
            next += c.clusters;
        }
}

template <class T>
void put(std::span<std::uint8_t> at, std::size_t off, T v) {
    for (std::size_t i = 0; i < sizeof(T); ++i)
        at[off + i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(v) >> (8 * i));
}

std::uint8_t checksum(const std::array<char, 11>& s) {
    std::uint8_t sum = 0;
    for (char c : s)
        sum = static_cast<std::uint8_t>(((sum & 1) ? 0x80 : 0) + (sum >> 1) + static_cast<std::uint8_t>(c));
    return sum;
}

void short_entry(std::span<std::uint8_t> e, const std::array<char, 11>& name, std::uint8_t attr,
                 std::uint32_t cluster, std::uint32_t size) {
    std::ranges::copy(name, e.begin());
    e[11] = attr;
    put<std::uint16_t>(e, 16, kEpoch);   // created
    put<std::uint16_t>(e, 18, kEpoch);   // accessed
    put<std::uint16_t>(e, 20, cluster >> 16);
    put<std::uint16_t>(e, 24, kEpoch);   // written
    put<std::uint16_t>(e, 26, cluster & 0xFFFF);
    put<std::uint32_t>(e, 28, size);
}

std::vector<std::uint8_t> directory(const node& dir, const node* parent, bool root, const std::string& label) {
    std::vector<std::uint8_t> out(std::size_t(dir.clusters) * kSector, 0);
    std::span<std::uint8_t> all(out);
    std::size_t at = 0;
    auto next = [&] { auto e = all.subspan(at, 32); at += 32; return e; };
    if (root) {
        std::array<char, 11> l;
        l.fill(' ');
        for (std::size_t i = 0; i < label.size() && i < 11; ++i)
            l[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[i])));
        short_entry(next(), l, 0x08, 0, 0);
    } else {
        std::array<char, 11> dot, dotdot;
        dot.fill(' ');
        dotdot.fill(' ');
        dot[0] = '.';
        dotdot[0] = dotdot[1] = '.';
        short_entry(next(), dot, 0x10, dir.first, 0);
        short_entry(next(), dotdot, 0x10, parent && parent->first != 2 ? parent->first : 0, 0);
    }
    for (const auto& c : dir.children) {
        if (c.long_name) {
            const auto count = long_entries(c);
            const auto sum = checksum(c.short_name);
            static constexpr std::array<std::size_t, 13> slot = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (std::uint32_t k = count; k >= 1; --k) {
                auto e = next();
                e[0] = static_cast<std::uint8_t>(k | (k == count ? 0x40 : 0));
                e[11] = 0x0F;
                e[13] = sum;
                for (std::size_t j = 0; j < 13; ++j) {
                    const std::size_t i = (k - 1) * 13 + j;
                    const char16_t u = i < c.name.size() ? c.name[i] : i == c.name.size() ? u'\0' : u'\xFFFF';
                    put<std::uint16_t>(e, slot[j], u);
                }
            }
        }
        short_entry(next(), c.short_name, c.dir ? 0x10 : 0x20, c.first, c.dir ? 0 : static_cast<std::uint32_t>(c.size));
    }
    return out;
}

}  // namespace

std::expected<void, std::string> write(const std::filesystem::path& root, const std::filesystem::path& image,
                                       const options& opts) {
    node tree;
    tree.host = root;
    tree.dir = true;
    if (auto r = read_tree(tree); !r) return r;
    name_children(tree);
    std::uint32_t next = 2;
    allocate(tree, true, next);
    const std::uint64_t used = next - 2;
    const std::uint64_t clusters64 = std::max<std::uint64_t>(kMinClusters, used + opts.free_bytes / kSector);
    const std::uint64_t fat64 = ((clusters64 + 2) * 4 + kSector - 1) / kSector;
    const std::uint64_t volume64 = kReserved + 2 * fat64 + clusters64;
    if (kPartitionStart + volume64 > 0xFFFFFFFFull)
        return std::unexpected(std::string("the tree is larger than a disk of 32-bit sectors"));
    const auto clusters = static_cast<std::uint32_t>(clusters64);
    const auto fat_sectors = static_cast<std::uint32_t>(fat64);
    const auto volume = static_cast<std::uint32_t>(volume64);

    std::fstream out(image, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!out)
        return std::unexpected("cannot write " + image.string());
    std::error_code ec;
    std::filesystem::resize_file(image, std::uint64_t(kPartitionStart + volume) * kSector, ec);
    if (ec)
        return std::unexpected("cannot size " + image.string() + ": " + ec.message());
    auto at = [&](std::uint64_t sector, std::span<const std::uint8_t> bytes) {
        out.seekp(static_cast<std::streamoff>(sector * kSector));
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };

    std::array<std::uint8_t, kSector> mbr{};
    put<std::uint32_t>(mbr, 440, 0x4e555244u);        // the disk's signature
    mbr[446 + 1] = 0xFE; mbr[446 + 2] = 0xFF; mbr[446 + 3] = 0xFF;   // CHS: beyond the CHS range
    mbr[446 + 4] = 0x0C;
    mbr[446 + 5] = 0xFE; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
    put<std::uint32_t>(mbr, 446 + 8, kPartitionStart);
    put<std::uint32_t>(mbr, 446 + 12, volume);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    at(0, mbr);

    std::array<std::uint8_t, kSector> boot{};
    boot[0] = 0xEB; boot[1] = 0x58; boot[2] = 0x90;
    std::ranges::copy(std::string_view("MSWIN4.1"), boot.begin() + 3);
    put<std::uint16_t>(boot, 11, kSector);
    boot[13] = 1;                                      // sectors a cluster
    put<std::uint16_t>(boot, 14, kReserved);
    boot[16] = 2;                                      // FATs
    boot[21] = 0xF8;                                   // a fixed disk
    put<std::uint16_t>(boot, 24, 63);
    put<std::uint16_t>(boot, 26, 255);
    put<std::uint32_t>(boot, 28, kPartitionStart);     // hidden sectors: those before the partition
    put<std::uint32_t>(boot, 32, volume);
    put<std::uint32_t>(boot, 36, fat_sectors);
    put<std::uint32_t>(boot, 44, 2);                   // the root directory's cluster
    put<std::uint16_t>(boot, 48, 1);                   // FSInfo
    put<std::uint16_t>(boot, 50, 6);                   // the boot sector's copy
    boot[64] = 0x80;
    boot[66] = 0x29;
    put<std::uint32_t>(boot, 67, 0x4e555244u);         // the volume's serial number
    std::array<char, 11> label;
    label.fill(' ');
    for (std::size_t i = 0; i < opts.label.size() && i < 11; ++i)
        label[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(opts.label[i])));
    std::ranges::copy(label, boot.begin() + 71);
    std::ranges::copy(std::string_view("FAT32   "), boot.begin() + 82);
    boot[510] = 0x55; boot[511] = 0xAA;

    std::array<std::uint8_t, kSector> info{};
    put<std::uint32_t>(info, 0, 0x41615252u);
    put<std::uint32_t>(info, 484, 0x61417272u);
    put<std::uint32_t>(info, 488, static_cast<std::uint32_t>(clusters - used));
    put<std::uint32_t>(info, 492, next);
    put<std::uint32_t>(info, 508, 0xAA550000u);
    for (std::uint32_t copy : {0u, 6u}) {
        at(kPartitionStart + copy, boot);
        at(kPartitionStart + copy + 1, info);
    }

    std::vector<std::uint32_t> fat(std::size_t(fat_sectors) * (kSector / 4), 0);
    fat[0] = 0x0FFFFFF8;
    fat[1] = kEnd;
    std::function<void(const node&)> chain = [&](const node& n) {
        for (std::uint32_t k = 0; k < n.clusters; ++k)
            fat[n.first + k] = k + 1 == n.clusters ? kEnd : n.first + k + 1;
        for (const auto& c : n.children) chain(c);
    };
    chain(tree);
    std::vector<std::uint8_t> fat_bytes(fat.size() * 4);
    for (std::size_t i = 0; i < fat.size(); ++i)
        put<std::uint32_t>(fat_bytes, i * 4, fat[i]);
    for (std::uint32_t copy = 0; copy < 2; ++copy)
        at(kPartitionStart + kReserved + copy * fat_sectors, fat_bytes);

    const std::uint64_t data = kPartitionStart + kReserved + 2ull * fat_sectors;
    std::vector<char> buffer(1 << 16);
    std::string failure;
    std::function<void(const node&, const node*, bool)> place = [&](const node& d, const node* parent, bool is_root) {
        at(data + (d.first - 2), directory(d, parent, is_root, opts.label));
        for (const auto& c : d.children) {
            if (c.dir) {
                place(c, &d, false);
            } else if (c.size > 0) {
                std::ifstream in(c.host, std::ios::binary);
                out.seekp(static_cast<std::streamoff>((data + (c.first - 2)) * kSector));
                std::uint64_t left = c.size;
                while (in && left > 0) {
                    in.read(buffer.data(), static_cast<std::streamsize>(std::min<std::uint64_t>(left, buffer.size())));
                    out.write(buffer.data(), in.gcount());
                    left -= static_cast<std::uint64_t>(in.gcount());
                }
                if (left > 0 && failure.empty())
                    failure = "cannot read " + c.host.string();
            }
        }
    };
    place(tree, nullptr, true);
    out.flush();
    if (!failure.empty())
        return std::unexpected(failure);
    if (!out)
        return std::unexpected("cannot write " + image.string());
    return {};
}

}  // namespace dirun::fat
