#include "virtual_keyboard.h"

#include "log.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace vtt {
namespace virtual_keyboard {
namespace {

// One fixed layout for the lifetime of the keyboard, like a real one has.
// Changing the keymap between words does not work in practice: compositor,
// input method and application pick the new keymap up at different moments,
// and keys pressed in between are read with the old one.
//
// The layout follows a Swedish keyboard (letters, digits and punctuation where
// they are on a real one, so shortcuts like Ctrl+V hit the V key), each key with a plain
// and a Shift level. A second layout group on the same keys holds brackets,
// typographic punctuation and other letters; switching group, like Shift, is
// keyboard state and not a new keymap. XKB keycodes are evdev codes + 8.
struct LayoutKey {
    int keycode;
    char32_t plain;
    char32_t shifted;
};
const LayoutKey kLayout[] = {
    {10, U'1', U'!'}, {11, U'2', U'"'}, {12, U'3', U'#'}, {13, U'4', U'$'},
    {14, U'5', U'%'}, {15, U'6', U'&'}, {16, U'7', U'/'}, {17, U'8', U'('},
    {18, U'9', U')'}, {19, U'0', U'='}, {20, U'+', U'?'}, {21, U'\'', U'*'},
    {24, U'q', U'Q'}, {25, U'w', U'W'}, {26, U'e', U'E'}, {27, U'r', U'R'},
    {28, U't', U'T'}, {29, U'y', U'Y'}, {30, U'u', U'U'}, {31, U'i', U'I'},
    {32, U'o', U'O'}, {33, U'p', U'P'}, {34, U'å', U'Å'}, {35, U'é', U'É'},
    {38, U'a', U'A'}, {39, U's', U'S'}, {40, U'd', U'D'}, {41, U'f', U'F'},
    {42, U'g', U'G'}, {43, U'h', U'H'}, {44, U'j', U'J'}, {45, U'k', U'K'},
    {46, U'l', U'L'}, {47, U'ö', U'Ö'}, {48, U'ä', U'Ä'},
    {49, U'@', U'€'}, {51, U'<', U'>'},
    {52, U'z', U'Z'}, {53, U'x', U'X'}, {54, U'c', U'C'}, {55, U'v', U'V'},
    {56, U'b', U'B'}, {57, U'n', U'N'}, {58, U'm', U'M'},
    {59, U',', U';'}, {60, U'.', U':'}, {61, U'-', U'_'},
    {65, U' ', U' '},
};

// Second group, on the first keys of kLayout in order.
const char32_t kSecondGroup[][2] = {
    {U'[', U']'}, {U'{', U'}'}, {U'|', U'\\'}, {U'~', U'^'}, {U'`', U'\u00b4'},
    {U'\u2013', U'\u2014'}, {U'\u2026', U'\u2022'}, {U'\u201c', U'\u201d'},
    {U'\u2018', U'\u2019'}, {U'\u00ab', U'\u00bb'}, {U'\u2192', U'\u2190'},
    {U'\u00a7', U'\u00b0'},
    {U'\u00fc', U'\u00dc'}, {U'\u00e8', U'\u00c8'}, {U'\u00e0', U'\u00c0'},
    {U'\u00e1', U'\u00c1'}, {U'\u00e7', U'\u00c7'}, {U'\u00f1', U'\u00d1'},
    {U'\u00f8', U'\u00d8'}, {U'\u00e6', U'\u00c6'}, {U'\u00ea', U'\u00ca'},
    {U'\u00eb', U'\u00cb'}, {U'\u00ef', U'\u00cf'}, {U'\u00f4', U'\u00d4'},
    {U'\u00f3', U'\u00d3'}, {U'\u00ed', U'\u00cd'}, {U'\u00fa', U'\u00da'},
    {U'\u00df', U'\u00b5'}, {U'\u00bd', U'\u00bc'}, {U'\u00d7', U'\u00f7'},
    {U'\u2264', U'\u2265'}, {U'\u2260', U'\u2248'}, {U'\u00b1', U'\u00a3'},
    {U'\u201e', U'\u201a'}, {U'\u2212', U'\u00b7'},
};
constexpr std::size_t kSecondGroupCount = sizeof(kSecondGroup) / sizeof(kSecondGroup[0]);

struct NamedKey {
    const char* name;
    int keycode;
    unsigned keysym;
};
const NamedKey kNamedKeys[] = {
    {"Escape", 9, 0xff1b},  {"BackSpace", 22, 0xff08}, {"Tab", 23, 0xff09},
    {"Return", 36, 0xff0d}, {"Home", 110, 0xff50},     {"Up", 111, 0xff52},
    {"Left", 113, 0xff51},  {"Right", 114, 0xff53},    {"End", 115, 0xff57},
    {"Down", 116, 0xff54},  {"Delete", 119, 0xffff},
};

constexpr unsigned kShift = 1;

unsigned keysym_for(char32_t character) {
    // Latin-1 keysyms equal the code point; the rest are 0x01000000 + code point.
    if (character >= 0x20 && character <= 0xff) return static_cast<unsigned>(character);
    return 0x01000000u + static_cast<unsigned>(character);
}

struct Stroke {
    int keycode = 0;
    bool shift = false;
    unsigned group = 0;
};

// Where a character is on the layout; keycode 0 when it is not on it.
Stroke stroke_for(char32_t character) {
    static const std::map<char32_t, Stroke> strokes = [] {
        std::map<char32_t, Stroke> table;
        for (std::size_t index = 0; index < kSecondGroupCount; ++index) {
            table.emplace(kSecondGroup[index][1], Stroke{kLayout[index].keycode, true, 1});
            table.emplace(kSecondGroup[index][0], Stroke{kLayout[index].keycode, false, 1});
        }
        for (const auto& key : kLayout) {
            table[key.shifted] = Stroke{key.keycode, true, 0};
            table[key.plain] = Stroke{key.keycode, false, 0};
        }
        // Spaces that should simply come out as a space.
        table[U' '] = table[U' '];
        return table;
    }();
    const auto found = strokes.find(character);
    return found == strokes.end() ? Stroke{} : found->second;
}

std::u32string decode(const std::string& text) {
    std::u32string result;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const int length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2
                         : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + static_cast<std::size_t>(length) > text.size()) { ++i; continue; }
        char32_t value = length == 1 ? lead : lead & (0xFF >> (length + 1));
        for (int k = 1; k < length; ++k) {
            value = (value << 6) | (static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]) & 0x3F);
        }
        result.push_back(value);
        i += static_cast<std::size_t>(length);
    }
    return result;
}

std::string build_keymap() {
    std::string keymap = "xkb_keymap {\nxkb_keycodes \"vtt\" {\nminimum = 8;\nmaximum = 255;\n";
    for (const auto& key : kLayout) {
        keymap += "<K" + std::to_string(key.keycode) + "> = " + std::to_string(key.keycode) + ";\n";
    }
    for (const auto& key : kNamedKeys) {
        keymap += "<K" + std::to_string(key.keycode) + "> = " + std::to_string(key.keycode) + ";\n";
    }
    keymap += "};\nxkb_types \"vtt\" { include \"complete\" };\n"
              "xkb_compatibility \"vtt\" { include \"complete\" };\n"
              "xkb_symbols \"vtt\" {\n";
    char buffer[256];
    for (std::size_t index = 0; index < sizeof(kLayout) / sizeof(kLayout[0]); ++index) {
        const auto& key = kLayout[index];
        if (index < kSecondGroupCount) {
            std::snprintf(buffer, sizeof(buffer),
                          "key <K%d> { type[Group1] = \"TWO_LEVEL\", type[Group2] = \"TWO_LEVEL\", "
                          "symbols[Group1] = [ 0x%x, 0x%x ], symbols[Group2] = [ 0x%x, 0x%x ] };\n",
                          key.keycode, keysym_for(key.plain), keysym_for(key.shifted),
                          keysym_for(kSecondGroup[index][0]), keysym_for(kSecondGroup[index][1]));
        } else {
            std::snprintf(buffer, sizeof(buffer),
                          "key <K%d> { type = \"TWO_LEVEL\", symbols[Group1] = [ 0x%x, 0x%x ] };\n",
                          key.keycode, keysym_for(key.plain), keysym_for(key.shifted));
        }
        keymap += buffer;
    }
    for (const auto& key : kNamedKeys) {
        std::snprintf(buffer, sizeof(buffer), "key <K%d> { [ 0x%x ] };\n", key.keycode, key.keysym);
        keymap += buffer;
    }
    keymap += "};\n};\n";
    return keymap;
}

class Keyboard {
public:
    ~Keyboard() {
        if (keyboard_) zwp_virtual_keyboard_v1_destroy(keyboard_);
        if (manager_) zwp_virtual_keyboard_manager_v1_destroy(manager_);
        if (seat_) wl_seat_destroy(seat_);
        if (registry_) wl_registry_destroy(registry_);
        if (display_) wl_display_disconnect(display_);
    }

    bool connect() {
        if (keyboard_) return true;
        if (failed_) return false;
        failed_ = true; // until everything below worked

        display_ = wl_display_connect(nullptr);
        if (!display_) {
            vtlog("virtual keyboard: no Wayland display");
            return false;
        }
        registry_ = wl_display_get_registry(display_);
        static const wl_registry_listener listener = {on_global, on_global_remove};
        wl_registry_add_listener(registry_, &listener, this);
        wl_display_roundtrip(display_);
        if (!seat_ || !manager_) {
            vtlog("virtual keyboard: the compositor has no virtual-keyboard protocol");
            return false;
        }
        keyboard_ = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(manager_, seat_);
        if (!keyboard_ || !upload_keymap()) return false;
        failed_ = false;
        return true;
    }

    bool type(const std::u32string& text, int delay_ms) {
        if (!connect()) return false;
        bool shifted = false;
        unsigned group = 0;
        for (const char32_t character : text) {
            const Stroke stroke = stroke_for(character);
            if (!stroke.keycode) continue;
            if (stroke.shift != shifted || stroke.group != group) {
                shifted = stroke.shift;
                group = stroke.group;
                zwp_virtual_keyboard_v1_modifiers(keyboard_, shifted ? kShift : 0, 0, 0, group);
            }
            tap(stroke.keycode);
            if (delay_ms > 0) {
                flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
        if (shifted || group) zwp_virtual_keyboard_v1_modifiers(keyboard_, 0, 0, 0, 0);
        return flush();
    }

    bool press(int keycode, unsigned modifiers, int count) {
        if (!connect()) return false;
        zwp_virtual_keyboard_v1_modifiers(keyboard_, modifiers, 0, 0, 0);
        for (int i = 0; i < count; ++i) tap(keycode);
        zwp_virtual_keyboard_v1_modifiers(keyboard_, 0, 0, 0, 0);
        return flush();
    }

private:
    static void on_global(void* data, wl_registry* registry, uint32_t name,
                          const char* interface, uint32_t version) {
        auto* self = static_cast<Keyboard*>(data);
        if (std::strcmp(interface, wl_seat_interface.name) == 0 && !self->seat_) {
            self->seat_ = static_cast<wl_seat*>(wl_registry_bind(
                registry, name, &wl_seat_interface, std::min<uint32_t>(version, 5)));
        } else if (std::strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0 &&
                   !self->manager_) {
            self->manager_ = static_cast<zwp_virtual_keyboard_manager_v1*>(wl_registry_bind(
                registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
        }
    }
    static void on_global_remove(void*, wl_registry*, uint32_t) {}

    bool upload_keymap() {
        const std::string keymap = build_keymap();
        const int fd = memfd_create("vtt-keymap", MFD_CLOEXEC);
        if (fd < 0) return false;
        const std::size_t size = keymap.size() + 1;
        if (ftruncate(fd, static_cast<off_t>(size)) != 0) { close(fd); return false; }
        void* memory = mmap(nullptr, size, PROT_WRITE, MAP_SHARED, fd, 0);
        if (memory == MAP_FAILED) { close(fd); return false; }
        std::memcpy(memory, keymap.c_str(), size);
        munmap(memory, size);

        zwp_virtual_keyboard_v1_keymap(keyboard_, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                       static_cast<uint32_t>(size));
        close(fd);
        return wl_display_roundtrip(display_) >= 0;
    }

    void tap(int keycode) {
        const auto now = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        const auto key = static_cast<uint32_t>(keycode - 8);
        zwp_virtual_keyboard_v1_key(keyboard_, now, key, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(keyboard_, now, key, WL_KEYBOARD_KEY_STATE_RELEASED);
    }

    bool flush() {
        if (wl_display_roundtrip(display_) < 0) {
            vtlog("virtual keyboard: lost the Wayland connection");
            failed_ = true;
            return false;
        }
        return true;
    }

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_seat* seat_ = nullptr;
    zwp_virtual_keyboard_manager_v1* manager_ = nullptr;
    zwp_virtual_keyboard_v1* keyboard_ = nullptr;
    bool failed_ = false;
};

std::mutex g_mutex;
Keyboard& keyboard() {
    static Keyboard instance;
    return instance;
}

unsigned modifier_mask(const std::vector<std::string>& modifiers) {
    unsigned mask = 0;
    for (const auto& modifier : modifiers) {
        if (modifier == "shift") mask |= 1;
        else if (modifier == "ctrl") mask |= 4;
        else if (modifier == "alt") mask |= 8;
        else if (modifier == "logo") mask |= 64;
    }
    return mask;
}

} // namespace

bool available() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return keyboard().connect();
}

bool can_type(const std::string& text) {
    for (const char32_t character : decode(text)) {
        if (!stroke_for(character).keycode) return false;
    }
    return true;
}

bool type_text(const std::string& text, int delay_ms) {
    if (text.empty()) return true;
    if (!can_type(text)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return keyboard().type(decode(text), delay_ms);
}

bool press_key(const std::string& key, const std::vector<std::string>& modifiers, int count) {
    if (count <= 0) return true;

    int keycode = 0;
    for (const auto& named : kNamedKeys) {
        if (key == named.name) keycode = named.keycode;
    }
    if (!keycode && key.size() == 1) {
        const Stroke stroke = stroke_for(static_cast<unsigned char>(key[0]));
        if (!stroke.shift && !stroke.group) keycode = stroke.keycode;
    }
    if (!keycode) {
        vtlog("virtual keyboard: unknown key %s", key.c_str());
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    return keyboard().press(keycode, modifier_mask(modifiers), count);
}

} // namespace virtual_keyboard
} // namespace vtt
