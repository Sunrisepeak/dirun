// The FAT32 disk (dirun.fat, 0.6.0): written from a tree, then read here by a reader of the
// format's own rules (the partition, the boot sector, the FAT's chains, long names), and, where
// mtools and dosfstools are on PATH, by them.
import std;
import mcpplibs.mtest;
import dirun.fat;

using namespace mcpplibs::mtest;

namespace {

struct volume {
    std::vector<std::uint8_t> bytes;
    std::uint64_t base = 0;  // the partition's first byte
    std::uint32_t reserved = 0, fats = 0, fat_sectors = 0, root = 0, total = 0;

    template <class T>
    T at(std::uint64_t off) const {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) v |= std::uint64_t(bytes[off + i]) << (8 * i);
        return static_cast<T>(v);
    }
    std::uint64_t cluster(std::uint32_t n) const {
        return base + (std::uint64_t(reserved) + std::uint64_t(fats) * fat_sectors + (n - 2)) * 512;
    }
    std::uint32_t next(std::uint32_t n) const { return at<std::uint32_t>(base + reserved * 512ull + n * 4ull) & 0x0FFFFFFF; }
    std::vector<std::uint8_t> chain(std::uint32_t first, std::uint64_t size) const {
        std::vector<std::uint8_t> out;
        for (std::uint32_t n = first; n >= 2 && n < 0x0FFFFFF8 && out.size() < size; n = next(n))
            out.insert(out.end(), bytes.begin() + cluster(n), bytes.begin() + cluster(n) + 512);
        out.resize(std::min<std::uint64_t>(size, out.size()));
        return out;
    }
};

struct entry {
    std::u16string name;
    std::string short_name;
    bool dir = false;
    std::uint32_t first = 0, size = 0;
};

std::vector<entry> list(const volume& v, std::uint32_t first) {
    std::vector<entry> out;
    std::u16string pending;
    const auto bytes = v.chain(first, std::uint64_t(1) << 32);
    for (std::size_t off = 0; off + 32 <= bytes.size(); off += 32) {
        const auto* e = bytes.data() + off;
        if (e[0] == 0) break;
        if (e[11] == 0x0F) {
            std::u16string part;
            for (std::size_t s : {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30}) {
                const char16_t c = static_cast<char16_t>(e[s] | (e[s + 1] << 8));
                if (c == 0 || c == 0xFFFF) break;
                part.push_back(c);
            }
            pending = part + pending;
            continue;
        }
        if (e[11] & 0x08) continue;  // the volume's label
        entry x;
        x.short_name = std::string(reinterpret_cast<const char*>(e), 11);
        x.dir = (e[11] & 0x10) != 0;
        x.first = std::uint32_t(e[26] | (e[27] << 8)) | (std::uint32_t(e[20] | (e[21] << 8)) << 16);
        x.size = std::uint32_t(e[28]) | (e[29] << 8) | (e[30] << 16) | (std::uint32_t(e[31]) << 24);
        x.name = pending;
        if (x.name.empty()) {   // an 8.3 name alone: BASE.EXT
            const auto trim = [](std::string t) { return t.substr(0, t.find_last_not_of(' ') + 1); };
            const auto base = trim(x.short_name.substr(0, 8)), ext = trim(x.short_name.substr(8));
            for (char c : base + (ext.empty() ? "" : "." + ext)) x.name.push_back(static_cast<char16_t>(c));
        }
        pending.clear();
        out.push_back(x);
    }
    return out;
}

const entry* find(const std::vector<entry>& es, std::u16string_view name) {
    for (const auto& e : es)
        if (e.name == name) return &e;
    return nullptr;
}

std::filesystem::path scratch(std::string_view name) {
    auto p = std::filesystem::temp_directory_path() / std::format("dirun-fat-test-{}", name);
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

volume load(const std::filesystem::path& image) {
    volume v;
    std::ifstream in(image, std::ios::binary);
    v.bytes.assign(std::istreambuf_iterator<char>(in), {});
    v.base = std::uint64_t(v.at<std::uint32_t>(446 + 8)) * 512;
    v.reserved = v.at<std::uint16_t>(v.base + 14);
    v.fats = v.bytes[v.base + 16];
    v.total = v.at<std::uint32_t>(v.base + 32);
    v.fat_sectors = v.at<std::uint32_t>(v.base + 36);
    v.root = v.at<std::uint32_t>(v.base + 44);
    return v;
}

std::string text(const std::vector<std::uint8_t>& b) { return std::string(b.begin(), b.end()); }

bool on_path(const std::string& tool) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    const char sep = std::filesystem::path::preferred_separator == '\\' ? ';' : ':';
    for (const auto part : std::views::split(std::string_view(path), sep))
        if (std::filesystem::exists(std::filesystem::path(std::string_view(part)) / tool)) return true;
    return false;
}

std::filesystem::path tree() {
    const auto root = scratch("tree");
    std::ofstream(root / "APP.EFI", std::ios::binary) << "MZ-application";
    std::ofstream(root / "startup.nsh", std::ios::binary) << "fs0:\r\nAPP.EFI\r\n";
    std::ofstream(root / "a name with blanks.data", std::ios::binary) << "blanks";
    std::ofstream(root / "empty", std::ios::binary);
    std::filesystem::create_directories(root / "share" / "deep");
    std::string big(3000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + i % 26);
    std::ofstream(root / "share" / "deep" / "three-clusters.txt", std::ios::binary) << big;
    std::ofstream(root / std::filesystem::path(u8"données.txt"), std::ios::binary) << "utf-8";
    for (int i = 0; i < 12; ++i)   // twelve names with one basis: aliases ~1 to ~9, then ~10 on
        std::ofstream(root / std::format("long-name-{:02}.txt", i), std::ios::binary) << i;
    return root;
}

}  // namespace

UTest disk{"the disk is an MBR with one FAT32 partition the format's rules read", [] {
    const auto root = tree();
    const auto image = scratch("disk") / "disk.img";
    auto w = dirun::fat::write(root, image);
    expect(fatal(w.has_value())) << (w ? "" : w.error());
    const auto v = load(image);
    expect(eq(int(v.bytes[510]), 0x55));
    expect(eq(int(v.bytes[446 + 4]), 0x0C));
    expect(eq(v.base, std::uint64_t(1) << 20));
    expect(eq(std::string(reinterpret_cast<const char*>(&v.bytes[v.base + 82]), 8), std::string("FAT32   ")));
    const std::uint32_t clusters = v.total - v.reserved - v.fats * v.fat_sectors;
    expect(ge(clusters, 65525u)) << "a FAT32 volume has at least 65525 clusters";
    expect(eq(v.bytes.size(), std::size_t(v.base + std::uint64_t(v.total) * 512)));
    expect(eq(v.at<std::uint32_t>(v.base + 512), 0x41615252u)) << "FSInfo";
}};

UTest names{"short names stay short, other names are long, aliases are unique", [] {
    const auto root = tree();
    const auto image = scratch("names") / "disk.img";
    expect(fatal(dirun::fat::write(root, image).has_value()));
    const auto v = load(image);
    const auto top = list(v, v.root);
    const auto* app = find(top, u"APP.EFI");
    expect(fatal(app != nullptr));
    expect(eq(text(v.chain(app->first, app->size)), std::string("MZ-application")));
    const auto* nsh = find(top, u"startup.nsh");
    expect(fatal(nsh != nullptr));
    expect(eq(nsh->short_name, std::string("STARTUP NSH"))) << "a name that differs only in case keeps its 8.3 form";
    expect(eq(text(v.chain(nsh->first, nsh->size)), std::string("fs0:\r\nAPP.EFI\r\n")));
    const auto* blanks = find(top, u"a name with blanks.data");
    expect(fatal(blanks != nullptr));
    expect(eq(blanks->short_name, std::string("ANAMEW~1DAT")));
    expect(fatal(find(top, u"données.txt") != nullptr));
    expect(eq(find(top, u"empty")->size, 0u));
    std::set<std::string> aliases;
    for (const auto& e : top) aliases.insert(e.short_name);
    expect(eq(aliases.size(), top.size())) << "every alias is distinct";
    expect(aliases.contains("LONG-N~1TXT"));
    expect(aliases.contains("LONG-~10TXT"));
}};

UTest nesting{"a directory's entries, its `.' and `..', and a file across clusters", [] {
    const auto root = tree();
    expect(fatal(eq(std::filesystem::file_size(root / "share" / "deep" / "three-clusters.txt"), std::uintmax_t(3000))))
        << "the tree's file was not written";
    const auto image = scratch("nesting") / "disk.img";
    expect(fatal(dirun::fat::write(root, image).has_value()));
    const auto v = load(image);
    const auto* share = find(list(v, v.root), u"share");
    expect(fatal(share != nullptr && share->dir));
    const auto inside = list(v, share->first);
    expect(fatal(inside.size() >= 3u));
    expect(eq(inside[0].short_name, std::string(".          ")));
    expect(eq(inside[0].first, share->first));
    expect(eq(inside[1].short_name, std::string("..         ")));
    expect(eq(inside[1].first, 0u)) << "`..' of a directory in the root names cluster 0";
    const auto* deep = find(inside, u"deep");
    expect(fatal(deep != nullptr));
    const auto* big = find(list(v, deep->first), u"three-clusters.txt");
    expect(fatal(big != nullptr));
    expect(eq(big->size, 3000u));
    const auto content = text(v.chain(big->first, big->size));
    expect(eq(content.size(), std::size_t(3000)));
    expect(eq(content.substr(2990), std::string("abcdefghij")));
}};

UTest tools{"mtools and fsck.fat read the disk (where they are on PATH)", [] {
    if (!on_path("mtype") || !on_path("fsck.fat")) {
        std::println("skipped: mtools or dosfstools is not on PATH");
        return;
    }
    const auto root = tree();
    const auto dir = scratch("tools");
    const auto image = dir / "disk.img";
    expect(fatal(dirun::fat::write(root, image).has_value()));
    const auto out = dir / "out.txt";
    const auto run = [&](const std::string& line) {
        return std::system(std::format("{} > \"{}\" 2>&1", line, out.string()).c_str());
    };
    expect(eq(run(std::format("MTOOLS_SKIP_CHECK=1 mtype -i \"{}@@1M\" \"::/share/deep/three-clusters.txt\"",
                              image.string())), 0));
    std::ifstream in(out);
    std::string got((std::istreambuf_iterator<char>(in)), {});
    expect(eq(got.size(), std::size_t(3000)));
    // fsck.fat reads a volume, not a disk: the partition is copied out first.
    const auto part = dir / "part.img";
    expect(eq(run(std::format("dd if=\"{}\" of=\"{}\" bs=1M skip=1", image.string(), part.string())), 0));
    expect(eq(run(std::format("fsck.fat -n \"{}\"", part.string())), 0)) << [&] {
        std::ifstream f(out);
        return std::string((std::istreambuf_iterator<char>(f)), {});
    }();
}};
