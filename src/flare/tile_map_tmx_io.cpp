// Tiled .tmx (XML) map format loader (Phase 5 split, 2026-09-15, extracted
// from tile_map.cpp). Custom strstr-based parser — no pugixml. Flare TMX has
// fixed structure: <map>, <tileset>, <layer>, <data encoding="csv">.
#include "tile_map_internal.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace md::flare {

// Read attribute value as integer from a tag element string.
static int XmlAttrInt(const char* elem, const char* attr) {
    char pat[64];
    snprintf(pat, sizeof(pat), " %s=\"", attr);
    const char* p = strstr(elem, pat);
    if (!p) return 0;
    return atoi(p + strlen(pat));
}

// Copy attribute value into buf (returns true if found).
static bool XmlAttrStr(const char* elem, const char* attr, char* buf, int n) {
    char pat[64];
    snprintf(pat, sizeof(pat), " %s=\"", attr);
    const char* p = strstr(elem, pat);
    if (!p) { buf[0] = '\0'; return false; }
    p += strlen(pat);
    int i = 0;
    for (; *p && *p != '"' && i < n - 1; ++i, ++p) buf[i] = *p;
    buf[i] = '\0';
    return true;
}

// Copy tag element text up to '>' (or up to max_len).
static int XmlElemText(const char* tag_start, char* out, int max_len) {
    const char* end = strchr(tag_start, '>');
    int len = end ? (int)(end - tag_start) : max_len - 1;
    if (len >= max_len) len = max_len - 1;
    memcpy(out, tag_start, len);
    out[len] = '\0';
    return len;
}

bool LoadTmx(const char* path, FlareMap& m) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    // 512 KB covers the largest known Flare TMX (104×104 × 3 layers ≈ 160 KB).
    static char buf[512 * 1024];
    int n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n <= 0) return false;
    buf[n] = '\0';

    memset(&m, 0, sizeof(m));

    // ── <map> ─────────────────────────────────────────────────────────────────
    const char* map_tag = strstr(buf, "<map ");
    if (!map_tag) return false;
    {
        char elem[512];
        XmlElemText(map_tag, elem, sizeof(elem));
        m.width  = XmlAttrInt(elem, "width");
        m.height = XmlAttrInt(elem, "height");
        m.tile_w = XmlAttrInt(elem, "tilewidth");
        m.tile_h = XmlAttrInt(elem, "tileheight");
    }

    // ── <properties> ─────────────────────────────────────────────────────────
    const char* props = strstr(buf, "<properties>");
    if (props) {
        const char* p = props;
        while ((p = strstr(p, "<property ")) != nullptr) {
            char pname[64] = {}, pval[128] = {};
            XmlAttrStr(p, "name",  pname, sizeof(pname));
            XmlAttrStr(p, "value", pval,  sizeof(pval));
            if      (strcmp(pname, "music")    == 0) CpStr(m.music_path,  sizeof(m.music_path),  pval);
            else if (strcmp(pname, "tileset")  == 0) CpStr(m.tileset_def, sizeof(m.tileset_def), pval);
            else if (strcmp(pname, "title")    == 0) CpStr(m.title,       sizeof(m.title),       pval);
            else if (strcmp(pname, "hero_pos") == 0) sscanf(pval, "%f,%f", &m.hero_x, &m.hero_y);
            p += 9;
        }
    }

    // ── <tileset> ─────────────────────────────────────────────────────────────
    const char* p = buf;
    while ((p = strstr(p, "<tileset ")) != nullptr) {
        if (m.tileset_count >= MAX_TILESETS) break;
        TileSet& ts = m.tilesets[m.tileset_count++];
        memset(&ts, 0, sizeof(ts));

        const char* tend = strstr(p, "</tileset>");
        if (!tend) tend = strstr(p, "/>");
        if (!tend) { ++p; continue; }

        char elem[1024];
        int elen = (int)(tend - p);
        if (elen >= (int)sizeof(elem)) elen = (int)sizeof(elem) - 1;
        memcpy(elem, p, elen);
        elem[elen] = '\0';

        ts.firstgid = XmlAttrInt(elem, "firstgid");
        ts.tile_w   = XmlAttrInt(elem, "tilewidth");
        ts.tile_h   = XmlAttrInt(elem, "tileheight");
        ts.columns  = XmlAttrInt(elem, "columns");

        const char* img = strstr(elem, "<image ");
        if (img) XmlAttrStr(img, "source", ts.image_path, sizeof(ts.image_path));

        p = tend + 1;
    }

    // ── <layer> ───────────────────────────────────────────────────────────────
    p = buf;
    while ((p = strstr(p, "<layer ")) != nullptr) {
        if (m.layer_count >= MAX_MAP_LAYERS) break;
        TileMapLayer& layer = m.layers[m.layer_count++];
        memset(&layer, 0, sizeof(layer));

        char lname[64] = {};
        XmlAttrStr(p, "name", lname, sizeof(lname));
        layer.type = LayerTypeOf(lname);

        const char* data_tag = strstr(p, "<data ");
        if (!data_tag) { ++p; continue; }
        const char* data_end = strstr(data_tag, "</data>");
        if (!data_end) { p = data_tag + 1; continue; }

        const char* csv = strchr(data_tag, '>');
        if (!csv || csv >= data_end) { p = data_tag + 1; continue; }
        ++csv;

        int count = 0;
        int max   = m.width * m.height;
        if (max > MAX_MAP_WIDTH * MAX_MAP_HEIGHT) max = MAX_MAP_WIDTH * MAX_MAP_HEIGHT;

        const char* q = csv;
        while (q < data_end && count < max) {
            while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') ++q;
            if (!*q || q >= data_end) break;
            long v = strtol(q, (char**)&q, 10);
            int row = count / m.width;
            int col = count % m.width;
            if (row < MAX_MAP_HEIGHT && col < MAX_MAP_WIDTH)
                layer.tiles[row * MAX_MAP_WIDTH + col] = (uint16_t)(v > 0 ? v : 0);
            ++count;
            if (*q == ',') ++q;
        }

        p = data_end + 7;
    }

    return m.width > 0 && m.height > 0;
}

} // namespace md::flare
