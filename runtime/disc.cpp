// Virtual GameCube disc: an extracted file tree (sys/ + files/), or a disc image (.iso / .gcm) read in place.
//
// The image needs no unpacking (phones have no room for a second 1.4 GB copy and no patience for one): its file table
// is read once, and every disc read becomes a seek into the image (Seg.foff). Mods work the same either way.
#include "sys.h"
#include "settings_ui.h"
#include <vector>
#include <algorithm>
#include <cstring>
#include <map>
#define NOMINMAX
#include "plat.h"

namespace {
struct Seg { uint64_t off, len; std::string path; uint64_t foff; const std::vector<uint8_t>* mem = nullptr; std::string name; };
std::vector<uint8_t> g_fst_patched;   // the file system table the game sees (mods may move / grow files)
std::vector<Seg> g_segs;
std::vector<uint8_t> g_sysbuf;   // not used
FILE* g_cur = nullptr;
std::string g_cur_path;
bool g_is_image = false;                      // the source is one file (an .iso / .gcm), not a folder
std::vector<uint8_t> g_image_fst;             // the image's own file table, as read (before mods)
std::map<std::string, std::pair<uint64_t, uint64_t>> g_sys;   // "main.dol" -> (offset in the source, size)

uint32_t be32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// A range of the disc image (0 bytes read is a failure: the image is cut short or damaged).
bool img_read(const std::string& path, uint64_t off, void* dst, size_t n) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  const bool ok = _fseeki64(f, (long long)off, SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
  fclose(f);
  return ok;
}

// The DOL's own size, from its section table (the same rule tools/game_import/gameimport.py uses).
uint64_t dol_size(const uint8_t* h) {
  uint64_t end = 0x100;
  for (int i = 0; i < 18; ++i) {
    const uint32_t off = be32(h + i * 4), sz = be32(h + 0x90 + i * 4);
    if (sz) end = std::max<uint64_t>(end, (uint64_t)off + sz);
  }
  return end;
}

bool slurp(const std::string& p, std::vector<uint8_t>& out) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(n);
  fread(out.data(), 1, n, f);
  fclose(f);
  return true;
}
}  // namespace

std::string g_disc_root;

const std::vector<uint8_t>& disc_fst() { return g_fst_patched; }

namespace {
// Mods: <exe dir>\modsiles\<same path as on the disc> replaces that disc file (model swaps, tools/model_swap.py). A
// replacement that is larger than the original is placed after the end of the disc and its file table entry is patched.
std::string mods_dir() {
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string d = exe;
  return d.substr(0, d.find_last_of("\\/")) + RT_S "mods" RT_S "files";
}
bool file_size(const std::string& p, uint64_t& n) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return false;
  _fseeki64(f, 0, SEEK_END);
  n = (uint64_t)_ftelli64(f);
  fclose(f);
  return true;
}
}  // namespace

bool disc_init(const std::string& root) {
  g_disc_root = root;
  std::vector<uint8_t> boot, bi2, fst;
  g_is_image = !plat::is_dir(root) && plat::exists(root);
  uint32_t dol_off = 0, fst_off = 0;
  uint64_t dol_len = 0;
  if (g_is_image) {
    // ---- a disc image: read its header, its file table and the sizes of the system files --------------
    if (!img_read(root, 0, boot.data(), 0), false) {}   // (no-op: boot is resized below)
    boot.resize(0x440);
    if (!img_read(root, 0, boot.data(), boot.size())) { logf("disc: %s is too small or unreadable\n", root.c_str()); return false; }
    if (be32(&boot[0x1C]) != 0xC2339F3D) { logf("disc: %s is not a GameCube disc image\n", root.c_str()); return false; }
    const std::string id((const char*)&boot[0], 6);
    logf("disc: image %s (%.6s rev %d)\n", root.c_str(), id.c_str(), boot[7]);
    bi2.resize(0x1C00);   // 0x440..0x2440
    img_read(root, 0x440, bi2.data(), bi2.size());
    dol_off = be32(&boot[0x420]);
    fst_off = be32(&boot[0x424]);
    const uint32_t fst_size = be32(&boot[0x428]);
    {
      uint8_t al[0x20] = {}, dolh[0x100] = {};
      if (!img_read(root, 0x2440, al, sizeof al) || !img_read(root, dol_off, dolh, sizeof dolh) || fst_size < 12) {
        logf("disc: the image is cut short (its boot files are unreadable)\n");
        return false;
      }
      g_segs.push_back({0x2440, 0x20ull + be32(al + 0x14) + be32(al + 0x18), root, 0x2440});   // apploader
      dol_len = dol_size(dolh);
      g_segs.push_back({dol_off, dol_len, root, dol_off});
      fst.resize(fst_size);
      if (!img_read(root, fst_off, fst.data(), fst.size())) { logf("disc: the image's file table is unreadable\n"); return false; }
      g_sys["boot.bin"] = {0, boot.size()};
      g_sys["bi2.bin"] = {0x440, bi2.size()};
      g_sys["apploader.img"] = {0x2440, g_segs[g_segs.size() - 2].len};
      g_sys["main.dol"] = {dol_off, dol_len};
      g_sys["fst.bin"] = {fst_off, fst.size()};
    }
    g_image_fst = fst;
  } else {
    if (!slurp(root + "/sys/boot.bin", boot) || boot.size() < 0x440) { logf("disc: missing sys/boot.bin\n"); return false; }
    slurp(root + "/sys/bi2.bin", bi2);
    slurp(root + "/sys/fst.bin", fst);
    dol_off = be32(&boot[0x420]);
    fst_off = be32(&boot[0x424]);
    g_sys["boot.bin"] = {0, boot.size()};
    g_sys["bi2.bin"] = {0, bi2.size()};
    g_segs.push_back({0, boot.size(), root + "/sys/boot.bin", 0});
    g_segs.push_back({0x440, bi2.size(), root + "/sys/bi2.bin", 0});
    g_segs.push_back({0x2440, 0, root + "/sys/apploader.img", 0});
    {
      uint64_t n = 0;
      if (file_size(root + "/sys/apploader.img", n)) g_segs.back().len = n;
      g_sys["apploader.img"] = {0, n};
    }
    {
      uint64_t n = 0;
      if (file_size(root + "/sys/main.dol", n)) {
        g_segs.push_back({dol_off, n, root + "/sys/main.dol", 0});
        g_sys["main.dol"] = {0, n};
      }
    }
    g_sys["fst.bin"] = {0, 0};
  }
  g_fst_patched = fst;
  const std::string mods = mods_dir();
  uint64_t disc_end = 0;   // end of the original disc data, for relocated mods
  int n_mods = 0;
  // (the file table segment is added after the walk: mods may patch it)
  // walk the FST
  if (fst.size() >= 12) {
    uint32_t n = be32(&fst[8]);
    const uint8_t* names = fst.data() + n * 12;
    for (uint32_t i = 1; i < n; ++i)   // where the original disc data ends (relocated mods go behind it)
      if (!fst[i * 12]) disc_end = std::max(disc_end, (uint64_t)be32(&fst[i * 12 + 4]) + be32(&fst[i * 12 + 8]));
    std::vector<std::string> dirstack;      // path components
    std::vector<uint32_t> dirend;           // end index for each dir level
    std::string cur;
    for (uint32_t i = 1; i < n; ++i) {
      while (!dirend.empty() && i >= dirend.back()) { dirend.pop_back(); dirstack.pop_back(); }
      const uint8_t* e = &fst[i * 12];
      bool isdir = e[0] != 0;
      uint32_t noff = ((e[1] << 16) | (e[2] << 8) | e[3]);
      std::string name = (const char*)(names + noff);
      if (isdir) {
        dirstack.push_back(name);
        dirend.push_back(be32(e + 8));
      } else {
        std::string rel;
        for (auto& d : dirstack) rel += d + RT_S "";
        rel += name;
        uint64_t off = be32(e + 4), len = be32(e + 8);
        // the folder's file, or a range of the image (Seek.foff)
        std::string p = g_is_image ? root : (root + "/files");
        if (!g_is_image) {
          for (auto& d : dirstack) p += "/" + d;
          p += "/" + name;
        }
        uint64_t foff = g_is_image ? off : 0;
        uint64_t msize = 0;
        std::string mp = mods + RT_S "" + rel;
        // #120 character skin: <exe dir>\skins\<id>\shadow.one (agents/claude/scripts/build_skins.py) is Shadow's model archive
        static const char* const kSkins[] = {"", "y", "g", "o", "mb", "mp", "super", "evil", "sonic"};
        if (rel == "character" RT_S "shadow.one" && settings::g.skin > 0 && settings::g.skin < 9) {
          const std::string sp = mods.substr(0, mods.size() - 10) + "skins" RT_S "" + kSkins[settings::g.skin] + RT_S "shadow.one";
          if (file_size(sp, msize) && msize) mp = sp;
          else logf("disc: skin %d: %s missing, Shadow's own model\n", settings::g.skin, sp.c_str());
        }
        if (file_size(mp, msize) && msize) {
          ++n_mods;
          foff = 0;   // a mod file is read from its own file
          if (msize <= len) {
            p = mp;   // fits: same place on the disc
          } else {
            // goes behind the original disc data (32 KiB aligned); patch the table entry the game reads
            off = (disc_end + 0x7FFF) & ~0x7FFFull;
            disc_end = off + msize;
            uint8_t* w = &g_fst_patched[i * 12];
            w[4] = off >> 24; w[5] = off >> 16; w[6] = off >> 8; w[7] = (uint8_t)off;
            w[8] = msize >> 24; w[9] = msize >> 16; w[10] = msize >> 8; w[11] = (uint8_t)msize;
            len = msize;
            p = mp;
          }
          logf("disc: mod %s (%llu bytes, was %u)\n", rel.c_str(), (unsigned long long)msize, be32(e + 8));
        }
        g_segs.push_back({off, len, p, foff, nullptr, rel});
      }
    }
  }
  // NEW files: a file under mods\files whose path the disc does not have (e.g. a chunk archive stg0100_50.one for a
  // new chunk) gets its own file-table entry, appended as the last child of its (existing) folder, data behind the disc
  if (g_fst_patched.size() >= 12) {
    auto lower = [](std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; };
    auto put32 = [](uint8_t* w, uint32_t v) { w[0] = v >> 24; w[1] = v >> 16; w[2] = v >> 8; w[3] = (uint8_t)v; };
    // index the current table: folder path -> entry index, and every file path
    auto scan = [&](std::map<std::string, uint32_t>& dirs, std::map<std::string, uint32_t>& files) {
      dirs.clear(); files.clear(); dirs[""] = 0;
      const uint32_t n = be32(&g_fst_patched[8]);
      const char* nm = (const char*)g_fst_patched.data() + n * 12;
      std::vector<std::string> stack; std::vector<uint32_t> ends;
      for (uint32_t i = 1; i < n; ++i) {
        while (!ends.empty() && i >= ends.back()) { ends.pop_back(); stack.pop_back(); }
        const uint8_t* e = &g_fst_patched[i * 12];
        std::string path; for (auto& d : stack) path += d + RT_S "";
        path += lower(nm + ((e[1] << 16) | (e[2] << 8) | e[3]));
        if (e[0]) { dirs[path] = i; stack.push_back(lower(nm + ((e[1] << 16) | (e[2] << 8) | e[3]))); ends.push_back(be32(e + 8)); }
        else files[path] = i;
      }
    };
    std::map<std::string, uint32_t> dirs, files;
    scan(dirs, files);
    std::vector<std::pair<std::string, uint64_t>> found;   // rel path (as on disk), size
    std::vector<std::string> todo{""};
    while (!todo.empty()) {
      const std::string rel = todo.back(); todo.pop_back();
      WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((mods + RT_S "" + rel + "*").c_str(), &fd);
      if (h == INVALID_HANDLE_VALUE) continue;
      do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) todo.push_back(rel + name + RT_S "");
        else found.push_back({rel + name, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow});
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
    int added = 0;
    for (const auto& [rel, size] : found) {
      if (!size || files.count(lower(rel))) continue;
      const size_t cut = rel.find_last_of('\\');
      const std::string dir = cut == std::string::npos ? "" : lower(rel.substr(0, cut)), base = cut == std::string::npos ? rel : rel.substr(cut + 1);
      auto d = dirs.find(dir);
      if (d == dirs.end()) { logf("disc: new mod file %s skipped (folder %s is not on the disc)\n", rel.c_str(), dir.c_str()); continue; }
      const uint32_t n = be32(&g_fst_patched[8]), dirIdx = d->second;
      const uint32_t pos = dirIdx ? be32(&g_fst_patched[dirIdx * 12 + 8]) : n;   // the folder's end = insertion index
      // ancestors of the new entry (the folder and its parents): their ranges must grow even when they end at pos
      std::vector<uint32_t> anc; for (uint32_t a = dirIdx; a; a = be32(&g_fst_patched[a * 12 + 4])) anc.push_back(a);
      std::vector<uint8_t> names(g_fst_patched.begin() + n * 12, g_fst_patched.end());
      std::vector<uint8_t> ents(g_fst_patched.begin(), g_fst_patched.begin() + n * 12);
      for (uint32_t i = 1; i < n; ++i) {
        uint8_t* e = &ents[i * 12];
        if (!e[0]) continue;
        const uint32_t parent = be32(e + 4), end = be32(e + 8);
        const bool ancestor = std::find(anc.begin(), anc.end(), i) != anc.end();
        if (end > pos || (end == pos && ancestor)) put32(e + 8, end + 1);
        if (i >= pos && parent >= pos) put32(e + 4, parent + 1);
      }
      put32(&ents[8], n + 1);
      const uint32_t noff = (uint32_t)names.size();
      names.insert(names.end(), base.begin(), base.end()); names.push_back(0);
      const uint64_t off = (disc_end + 0x7FFF) & ~0x7FFFull; disc_end = off + size;
      uint8_t ne[12] = {0, (uint8_t)(noff >> 16), (uint8_t)(noff >> 8), (uint8_t)noff};
      put32(ne + 4, (uint32_t)off); put32(ne + 8, (uint32_t)size);
      ents.insert(ents.begin() + pos * 12, ne, ne + 12);
      g_fst_patched = ents; g_fst_patched.insert(g_fst_patched.end(), names.begin(), names.end());
      g_segs.push_back({off, size, mods + RT_S "" + rel, 0});
      logf("disc: NEW mod file %s (%llu bytes) added to the file table\n", rel.c_str(), (unsigned long long)size);
      ++added; scan(dirs, files);
    }
    if (added) n_mods += added;
  }
  {
    Seg fs{fst_off, g_fst_patched.size(), "", 0};
    fs.mem = &g_fst_patched;
    g_segs.push_back(fs);
  }
  std::sort(g_segs.begin(), g_segs.end(), [](const Seg& a, const Seg& b) { return a.off < b.off; });
  logf("disc: %zu segments\n", g_segs.size());
  return true;
}

bool disc_read(uint64_t offset, uint8_t* dst, uint32_t len) {
  memset(dst, 0, len);
  uint32_t done = 0;
  while (done < len) {
    uint64_t pos = offset + done;
    // find segment containing pos
    size_t lo = 0, hi = g_segs.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_segs[m].off <= pos) lo = m + 1; else hi = m; }
    const Seg* s = lo ? &g_segs[lo - 1] : nullptr;
    if (s && pos < s->off + s->len) {
      uint64_t in = pos - s->off;
      uint32_t n = (uint32_t)std::min<uint64_t>(len - done, s->len - in);
      if (s->mem) {
        if (in < s->mem->size()) memcpy(dst + done, s->mem->data() + in, std::min<uint64_t>(n, s->mem->size() - in));
        done += n;
        continue;
      }
      if (g_cur_path != s->path) {
        if (g_cur) fclose(g_cur);
        g_cur = fopen(s->path.c_str(), "rb");
        g_cur_path = s->path;
        if (!g_cur) logf("disc: cannot open %s\n", s->path.c_str());
      }
      if (g_cur) {
        _fseeki64(g_cur, (long long)(s->foff + in), SEEK_SET);
        fread(dst + done, 1, n, g_cur);
      }
      done += n;
    } else {
      // gap: skip to next segment start or end
      uint64_t next = lo < g_segs.size() ? g_segs[lo].off : pos + (len - done);
      uint32_t n = (uint32_t)std::min<uint64_t>(len - done, next - pos);
      if (n == 0) n = len - done;
      done += n;
    }
  }
  return true;
}

std::string disc_describe(uint64_t offset) {
  size_t lo = 0, hi = g_segs.size();
  while (lo < hi) { size_t m = (lo + hi) / 2; if (g_segs[m].off <= offset) lo = m + 1; else hi = m; }
  if (!lo) return "?";
  const Seg& s = g_segs[lo - 1];
  if (offset >= s.off + s.len) return "(gap)";
  if (!s.name.empty()) return s.name;   // the disc's own path ("files/stg0100.one", "character/shadow.one")
  std::string p = s.path;
  size_t k = p.find("/files/");
  if (k != std::string::npos) p = p.substr(k + 7);
  return p;
}

bool disc_seg_range(uint64_t offset, uint64_t* start, uint64_t* len) {
  size_t lo = 0, hi = g_segs.size();
  while (lo < hi) { size_t m = (lo + hi) / 2; if (g_segs[m].off <= offset) lo = m + 1; else hi = m; }
  if (!lo) return false;
  const Seg& s = g_segs[lo - 1];
  if (offset >= s.off + s.len) return false;
  *start = s.off; *len = s.len;
  return true;
}

bool disc_is_image() { return g_is_image; }

// One of the boot files (sys/main.dol and friends) from whichever source the disc came from: boot.cpp used to open
// root/sys/<name> directly, which only works for an extracted folder.
bool disc_sys_file(const char* name, std::vector<uint8_t>& out) {
  out.clear();
  if (!g_is_image) {
    return slurp(plat::join(g_disc_root, std::string("sys") + RT_S + name), out);
  }
  auto it = g_sys.find(name);
  if (it == g_sys.end()) return false;
  const uint64_t off = it->second.first, len = it->second.second;
  if (!len) return false;
  out.resize(len);
  return img_read(g_disc_root, off, out.data(), out.size());
}
