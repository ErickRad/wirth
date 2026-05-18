#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace util {

static std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("falha ao abrir arquivo: " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void write_text(const fs::path& path, const std::string& text) {
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("falha ao escrever arquivo: " + path.string());
    }
    out << text;
    if (!out) {
        throw std::runtime_error("falha ao finalizar escrita: " + path.string());
    }
}

static std::vector<std::string> read_lines(const fs::path& path) {
    std::string text = read_text(path);
    std::vector<std::string> lines;
    std::string current;
    for (char ch : text) {
        if (ch == '\n') {
            if (!current.empty() && current.back() == '\r') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty() || (!text.empty() && text.back() == '\n')) {
        if (!current.empty() && current.back() == '\r') {
            current.pop_back();
        }
        lines.push_back(current);
    }
    return lines;
}

static std::string trim(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

static std::string strip_comment(const std::string& line) {
    std::size_t cut = std::string::npos;
    for (char marker : {';', '#'}) {
        std::size_t pos = line.find(marker);
        if (pos != std::string::npos && (cut == std::string::npos || pos < cut)) {
            cut = pos;
        }
    }
    return cut == std::string::npos ? line : line.substr(0, cut);
}

static std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    char quote = 0;
    for (char ch : line) {
        if (quote != 0) {
            current.push_back(ch);
            if (ch == quote) {
                quote = 0;
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            current.push_back(ch);
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

static std::vector<std::string> split_args(const std::string& text) {
    std::vector<std::string> args;
    std::string current;
    char quote = 0;
    for (char ch : text) {
        if (quote != 0) {
            if (ch == quote) {
                quote = 0;
            } else {
                current.push_back(ch);
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            continue;
        }
        if (ch == ',') {
            std::string item = trim(current);
            if (!item.empty()) {
                args.push_back(item);
            }
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    std::string item = trim(current);
    if (!item.empty()) {
        args.push_back(item);
    }
    return args;
}

static bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

static long long parse_number(const std::string& text) {
    std::string s = trim(text);
    if (s.empty()) {
        throw std::runtime_error("número vazio");
    }
    int sign = 1;
    std::size_t pos = 0;
    if (s[pos] == '+' || s[pos] == '-') {
        if (s[pos] == '-') {
            sign = -1;
        }
        ++pos;
    }
    int base = 10;
    std::string_view rest(s.data() + pos, s.size() - pos);
    if (starts_with(rest, "0x")) {
        base = 16;
        pos += 2;
    } else if (starts_with(rest, "0b")) {
        base = 2;
        pos += 2;
    } else if (starts_with(rest, "0o")) {
        base = 8;
        pos += 2;
    }
    if (pos >= s.size()) {
        throw std::runtime_error("número inválido: " + text);
    }
    long long value = 0;
    for (; pos < s.size(); ++pos) {
        char ch = s[pos];
        if (ch == '_') {
            continue;
        }
        int digit = -1;
        if (ch >= '0' && ch <= '9') {
            digit = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            digit = 10 + (ch - 'a');
        } else if (ch >= 'A' && ch <= 'F') {
            digit = 10 + (ch - 'A');
        }
        if (digit < 0 || digit >= base) {
            throw std::runtime_error("número inválido: " + text);
        }
        value = value * base + digit;
    }
    return value * sign;
}

static bool is_number(const std::string& text) {
    try {
        (void)parse_number(text);
        return true;
    } catch (...) {
        return false;
    }
}

static int reg_index(const std::string& name) {
    std::string s = trim(name);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (s.size() < 2 || s[0] != 'R') {
        throw std::runtime_error("registrador inválido: " + name);
    }
    int idx = std::stoi(s.substr(1));
    if (idx < 0 || idx > 7) {
        throw std::runtime_error("registrador fora de faixa: " + name);
    }
    return idx;
}

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(ch >> 4) & 0xF]);
                    out.push_back(hex[ch & 0xF]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return out;
}

}  // namespace util

namespace json {

struct Value {
    using array_t = std::vector<Value>;
    using object_t = std::map<std::string, Value>;
    using storage_t = std::variant<std::nullptr_t, bool, long long, double, std::string, array_t, object_t>;

    storage_t data = nullptr;

    Value() = default;
    Value(std::nullptr_t) : data(nullptr) {}
    Value(bool v) : data(v) {}
    Value(long long v) : data(v) {}
    Value(int v) : data(static_cast<long long>(v)) {}
    Value(double v) : data(v) {}
    Value(const char* v) : data(std::string(v)) {}
    Value(std::string v) : data(std::move(v)) {}
    Value(array_t v) : data(std::move(v)) {}
    Value(object_t v) : data(std::move(v)) {}

    bool is_object() const { return std::holds_alternative<object_t>(data); }
    bool is_array() const { return std::holds_alternative<array_t>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_number() const { return std::holds_alternative<long long>(data) || std::holds_alternative<double>(data); }

    const object_t& as_object() const { return std::get<object_t>(data); }
    object_t& as_object() { return std::get<object_t>(data); }
    const array_t& as_array() const { return std::get<array_t>(data); }
    array_t& as_array() { return std::get<array_t>(data); }
    const std::string& as_string() const { return std::get<std::string>(data); }
    long long as_int() const {
        if (std::holds_alternative<long long>(data)) {
            return std::get<long long>(data);
        }
        if (std::holds_alternative<double>(data)) {
            return static_cast<long long>(std::get<double>(data));
        }
        throw std::runtime_error("valor JSON não numérico");
    }
    bool as_bool() const { return std::get<bool>(data); }

    Value& operator[](const std::string& key) { return std::get<object_t>(data)[key]; }
    const Value& at(const std::string& key) const { return std::get<object_t>(data).at(key); }
    bool contains(const std::string& key) const {
        if (!is_object()) {
            return false;
        }
        const auto& obj = std::get<object_t>(data);
        return obj.find(key) != obj.end();
    }
};

class Parser {
public:
    explicit Parser(std::string text) : text_(std::move(text)) {}

    Value parse() {
        skip_ws();
        Value value = parse_value();
        skip_ws();
        if (pos_ != text_.size()) {
            throw std::runtime_error("JSON com lixo após o fim do valor");
        }
        return value;
    }

private:
    std::string text_;
    std::size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    char get() {
        if (pos_ >= text_.size()) {
            throw std::runtime_error("JSON inesperadamente encerrado");
        }
        return text_[pos_++];
    }

    void expect(char ch) {
        char got = get();
        if (got != ch) {
            throw std::runtime_error(std::string("esperado '") + ch + "'");
        }
    }

    void expect_literal(std::string_view literal) {
        for (char ch : literal) {
            if (get() != ch) {
                throw std::runtime_error("literal JSON inválido");
            }
        }
    }

    Value parse_value() {
        skip_ws();
        char ch = peek();
        if (ch == '{') {
            return parse_object();
        }
        if (ch == '[') {
            return parse_array();
        }
        if (ch == '"') {
            return Value(parse_string());
        }
        if (ch == 't') {
            expect_literal("true");
            return Value(true);
        }
        if (ch == 'f') {
            expect_literal("false");
            return Value(false);
        }
        if (ch == 'n') {
            expect_literal("null");
            return Value(nullptr);
        }
        return parse_number();
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            char ch = get();
            if (ch == '"') {
                break;
            }
            if (ch == '\\') {
                char esc = get();
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = get();
                            code <<= 4;
                            if (h >= '0' && h <= '9') code += static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') code += static_cast<unsigned>(10 + h - 'a');
                            else if (h >= 'A' && h <= 'F') code += static_cast<unsigned>(10 + h - 'A');
                            else throw std::runtime_error("escape JSON inválido");
                        }
                        if (code <= 0x7F) {
                            out.push_back(static_cast<char>(code));
                        } else {
                            throw std::runtime_error("unicode fora do suporte mínimo do parser JSON");
                        }
                        break;
                    }
                    default:
                        throw std::runtime_error("escape JSON inválido");
                }
                continue;
            }
            out.push_back(ch);
        }
        return out;
    }

    Value parse_number() {
        std::size_t start = pos_;
        if (peek() == '-' || peek() == '+') {
            ++pos_;
        }
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            ++pos_;
        }
        bool is_float = false;
        if (peek() == '.') {
            is_float = true;
            ++pos_;
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                ++pos_;
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                ++pos_;
            }
        }
        std::string slice = text_.substr(start, pos_ - start);
        if (slice.empty()) {
            throw std::runtime_error("número JSON inválido");
        }
        if (is_float) {
            return Value(std::stod(slice));
        }
        return Value(std::stoll(slice));
    }

    Value parse_array() {
        expect('[');
        Value::array_t arr;
        skip_ws();
        if (peek() == ']') {
            ++pos_;
            return Value(std::move(arr));
        }
        while (true) {
            arr.push_back(parse_value());
            skip_ws();
            char ch = get();
            if (ch == ']') {
                break;
            }
            if (ch != ',') {
                throw std::runtime_error("array JSON inválido");
            }
            skip_ws();
        }
        return Value(std::move(arr));
    }

    Value parse_object() {
        expect('{');
        Value::object_t obj;
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return Value(std::move(obj));
        }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            obj.emplace(std::move(key), parse_value());
            skip_ws();
            char ch = get();
            if (ch == '}') {
                break;
            }
            if (ch != ',') {
                throw std::runtime_error("objeto JSON inválido");
            }
            skip_ws();
        }
        return Value(std::move(obj));
    }
};

static Value parse_text(const std::string& text) { return Parser(text).parse(); }
static Value parse_file(const fs::path& path) { return parse_text(util::read_text(path)); }

static std::string indent(int n) { return std::string(n, ' '); }

static std::string dump(const Value& value, int depth = 0, int step = 2) {
    struct Visitor {
        int depth;
        int step;
        std::string operator()(std::nullptr_t) const { return "null"; }
        std::string operator()(bool v) const { return v ? "true" : "false"; }
        std::string operator()(long long v) const { return std::to_string(v); }
        std::string operator()(double v) const { std::ostringstream ss; ss << v; return ss.str(); }
        std::string operator()(const std::string& s) const { return '"' + util::json_escape(s) + '"'; }
        std::string operator()(const Value::array_t& arr) const {
            if (arr.empty()) return "[]";
            std::string out = "[\n";
            for (std::size_t i = 0; i < arr.size(); ++i) {
                out += indent(depth + step) + dump(arr[i], depth + step, step);
                if (i + 1 < arr.size()) out += ',';
                out += '\n';
            }
            out += indent(depth) + ']';
            return out;
        }
        std::string operator()(const Value::object_t& obj) const {
            if (obj.empty()) return "{}";
            std::string out = "{\n";
            std::size_t i = 0;
            for (const auto& [key, val] : obj) {
                out += indent(depth + step) + '"' + util::json_escape(key) + "\": " + dump(val, depth + step, step);
                if (++i < obj.size()) out += ',';
                out += '\n';
            }
            out += indent(depth) + '}';
            return out;
        }
    } visitor{depth, step};
    return std::visit(visitor, value.data);
}

}  // namespace json

class ToolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct MacroDef {
    std::string name;
    std::vector<std::string> params;
    std::vector<std::string> body;
};

class MacroProcessor {
public:
    void process_file(const fs::path& src, const fs::path& dst) {
        util::write_text(dst, join_lines(process_lines(util::read_lines(src))));
    }

    std::vector<std::string> process_lines(const std::vector<std::string>& source_lines) {
        std::vector<std::string> output;
        for (std::size_t i = 0; i < source_lines.size();) {
            std::string no_comment = util::trim(util::strip_comment(source_lines[i]));
            auto tokens = util::tokenize(no_comment);
            if (tokens.empty()) {
                ++i;
                continue;
            }
            if (upper(tokens[0]) == "MACRO") {
                i = consume_macro_def(source_lines, i);
                continue;
            }
            auto expanded = expand_line(no_comment, 0);
            output.insert(output.end(), expanded.begin(), expanded.end());
            ++i;
        }
        return output;
    }

private:
    std::unordered_map<std::string, MacroDef> macros_;
    const int expansion_guard_ = 1024;

    static std::string upper(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        return s;
    }

    std::size_t consume_macro_def(const std::vector<std::string>& source_lines, std::size_t start_idx) {
        auto header_tokens = util::tokenize(util::strip_comment(source_lines[start_idx]));
        if (header_tokens.size() < 2) {
            throw ToolError("MACRO sem nome na linha " + std::to_string(start_idx + 1));
        }
        std::string name = header_tokens[1];
        std::vector<std::string> params;
        if (header_tokens.size() > 2) {
            std::string arg_text;
            for (std::size_t i = 2; i < header_tokens.size(); ++i) {
                if (i > 2) arg_text.push_back(' ');
                arg_text += header_tokens[i];
            }
            params = util::split_args(arg_text);
        }

        std::vector<std::string> body;
        int nested_level = 0;
        std::size_t i = start_idx + 1;
        while (i < source_lines.size()) {
            std::string line = util::trim(util::strip_comment(source_lines[i]));
            auto tk = util::tokenize(line);
            if (!tk.empty() && upper(tk[0]) == "MACRO") {
                ++nested_level;
            } else if (!tk.empty() && upper(tk[0]) == "MEND") {
                if (nested_level == 0) {
                    break;
                }
                --nested_level;
            }
            body.push_back(line);
            ++i;
        }

        if (i >= source_lines.size()) {
            throw ToolError("MEND ausente para macro " + name);
        }

        macros_[name] = MacroDef{name, params, process_lines(body)};
        return i + 1;
    }

    std::vector<std::string> expand_line(const std::string& line, int depth) {
        if (depth > expansion_guard_) {
            throw ToolError("expansão de macro excedeu limite (possível recursão infinita)");
        }
            auto tokens = util::tokenize(line);
        if (tokens.empty()) {
            return {};
        }
        const std::string& first = tokens[0];
        auto it = macros_.find(first);
        if (it == macros_.end()) {
            return {line};
        }

        const MacroDef& def = it->second;
        std::size_t pos = line.find(first);
        std::string arg_text = pos == std::string::npos ? std::string{} : util::trim(line.substr(pos + first.size()));
        std::vector<std::string> args = arg_text.empty() ? std::vector<std::string>{} : util::split_args(arg_text);
        if (args.size() != def.params.size()) {
            throw ToolError("macro " + def.name + ": esperado " + std::to_string(def.params.size()) +
                            " args, recebido " + std::to_string(args.size()));
        }
        std::unordered_map<std::string, std::string> mapping;
        for (std::size_t i = 0; i < args.size(); ++i) {
            mapping[def.params[i]] = args[i];
        }

        std::vector<std::string> expanded;
        for (std::string body_line : def.body) {
            for (const auto& [param, arg] : mapping) {
                replace_all(body_line, "{" + param + "}", arg);
                replace_all(body_line, "$" + param, arg);
            }
            auto nested = expand_line(body_line, depth + 1);
            expanded.insert(expanded.end(), nested.begin(), nested.end());
        }
        return expanded;
    }

    static void replace_all(std::string& text, const std::string& from, const std::string& to) {
        if (from.empty()) return;
        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    static std::string join_lines(const std::vector<std::string>& lines) {
        std::string text;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            text += lines[i];
            if (i + 1 < lines.size()) {
                text.push_back('\n');
            }
        }
        if (!text.empty()) {
            text.push_back('\n');
        }
        return text;
    }
};

struct Reloc {
    long long offset = 0;
    long long size = 0;
    std::string symbol;
    long long addend = 0;
};

struct ObjectFile {
    std::string module;
    long long size = 0;
    std::vector<std::uint8_t> code;
    std::map<std::string, long long> symbols;
    std::vector<std::string> globals;
    std::vector<std::string> externs;
    long long entry = 0;
    std::vector<Reloc> relocations;
};

static ObjectFile parse_object(const json::Value& root) {
    if (!root.is_object()) {
        throw ToolError("objeto JSON inválido");
    }
    const auto& obj = root.as_object();
    auto format_it = obj.find("format");
    if (format_it == obj.end() || !format_it->second.is_string() || format_it->second.as_string() != "wirth-obj-v1") {
        throw ToolError("arquivo de objeto inválido");
    }

    ObjectFile out;
    out.module = obj.at("module").as_string();
    out.size = obj.at("size").as_int();
    for (const auto& item : obj.at("code").as_array()) {
        out.code.push_back(static_cast<std::uint8_t>(item.as_int() & 0xFF));
    }
    for (const auto& [name, value] : obj.at("symbols").as_object()) {
        out.symbols[name] = value.as_int();
    }
    for (const auto& item : obj.at("globals").as_array()) {
        out.globals.push_back(item.as_string());
    }
    if (obj.contains("externs")) {
        for (const auto& item : obj.at("externs").as_array()) {
            out.externs.push_back(item.as_string());
        }
    }
    if (obj.contains("entry")) {
        out.entry = obj.at("entry").as_int();
    }
    if (obj.contains("relocations")) {
        for (const auto& item : obj.at("relocations").as_array()) {
            const auto& ro = item.as_object();
            Reloc r;
            r.offset = ro.at("offset").as_int();
            r.size = ro.at("size").as_int();
            r.symbol = ro.at("symbol").as_string();
            if (ro.find("addend") != ro.end()) {
                r.addend = ro.at("addend").as_int();
            }
            out.relocations.push_back(r);
        }
    }
    return out;
}

class TwoPassAssembler {
public:
    json::Value assemble_lines(const std::vector<std::string>& lines, const std::string& module_name, const std::string& source_name) {
        std::map<std::string, long long> symbols;
        std::vector<std::string> globals_export;
        std::vector<std::string> externs;
        std::vector<std::pair<long long, std::string>> parsed;

        long long pc = 0;
        for (long long line_no = 1; line_no <= static_cast<long long>(lines.size()); ++line_no) {
            std::string line = util::trim(util::strip_comment(lines[static_cast<std::size_t>(line_no - 1)]));
            if (line.empty()) {
                continue;
            }
            auto [label, rest] = split_label(line);
            if (!label.empty()) {
                if (symbols.find(label) != symbols.end()) {
                    throw ToolError(source_name + ":" + std::to_string(line_no) + ": símbolo duplicado " + label);
                }
                symbols[label] = pc;
                if (rest.empty()) {
                    continue;
                }
                line = rest;
            }

            parsed.emplace_back(line_no, line);
            auto tk = util::tokenize(line);
            if (tk.empty()) {
                continue;
            }
            std::string directive = upper(tk[0]);
            if (directive == ".GLOBAL") {
                if (tk.size() < 2) {
                    throw ToolError(source_name + ":" + std::to_string(line_no) + ": .GLOBAL exige símbolo");
                }
                globals_export.push_back(tk[1]);
                continue;
            }
            if (directive == ".EXTERN") {
                if (tk.size() < 2) {
                    throw ToolError(source_name + ":" + std::to_string(line_no) + ": .EXTERN exige símbolo");
                }
                externs.push_back(tk[1]);
                continue;
            }
            if (directive == ".WORD") {
                pc += 2;
                continue;
            }
            if (directive == ".SPACE") {
                if (tk.size() < 2) {
                    throw ToolError(source_name + ":" + std::to_string(line_no) + ": .SPACE exige quantidade");
                }
                pc += util::parse_number(tk[1]);
                continue;
            }
            if (directive == ".BYTE") {
                pc += 1;
                continue;
            }
            if (directive == ".ENTRY") {
                continue;
            }
            if (kInstrSizes.find(directive) == kInstrSizes.end()) {
                throw ToolError(source_name + ":" + std::to_string(line_no) + ": instrução desconhecida " + tk[0]);
            }
            pc += kInstrSizes.at(directive);
        }

        std::vector<std::uint8_t> code(static_cast<std::size_t>(pc), 0);
        std::vector<Reloc> relocations;
        long long entry = 0;
        pc = 0;
        for (const auto& [line_no, line] : parsed) {
            auto tk = util::tokenize(line);
            if (tk.empty()) {
                continue;
            }
            std::string op = upper(tk[0]);
            if (op == ".GLOBAL" || op == ".EXTERN") {
                continue;
            }
            if (op == ".ENTRY") {
                if (tk.size() < 2) {
                    throw ToolError(source_name + ":" + std::to_string(line_no) + ": .ENTRY exige símbolo");
                }
                std::string sym = tk[1];
                auto it = symbols.find(sym);
                if (it == symbols.end()) {
                    throw ToolError(source_name + ":" + std::to_string(line_no) + ": símbolo de entry ausente " + sym);
                }
                entry = it->second;
                continue;
            }
            if (op == ".BYTE") {
                if (tk.size() < 2) {
                    throw ToolError(source_name + ":" + std::to_string(line_no) + ": .BYTE exige valor");
                }
                code[static_cast<std::size_t>(pc)] = static_cast<std::uint8_t>(util::parse_number(tk[1]) & 0xFF);
                ++pc;
                continue;
            }
            if (op == ".WORD") {
                if (tk.size() < 2) {
                    throw ToolError(source_name + ":" + std::to_string(line_no) + ": .WORD exige valor/símbolo");
                }
                std::string value = tk[1];
                if (util::is_number(value)) {
                    emit_u16(code, pc, util::parse_number(value));
                } else {
                    emit_symbol_u16(code, pc, value, symbols, externs, relocations, source_name, line_no);
                }
                pc += 2;
                continue;
            }
            if (op == ".SPACE") {
                if (tk.size() < 2) {
                    throw ToolError(source_name + ":" + std::to_string(line_no) + ": .SPACE exige quantidade");
                }
                long long count = util::parse_number(tk[1]);
                for (long long i = 0; i < count; ++i) {
                    code[static_cast<std::size_t>(pc + i)] = 0;
                }
                pc += count;
                continue;
            }

            std::string args_text = util::trim(line.substr(tk[0].size()));
            auto args = util::split_args(args_text);
            pc = encode_instruction(code, pc, op, args, symbols, externs, relocations, source_name, line_no);
        }

        std::vector<std::string> undefined_globals;
        for (const auto& g : globals_export) {
            if (symbols.find(g) == symbols.end()) {
                undefined_globals.push_back(g);
            }
        }
        if (!undefined_globals.empty()) {
            std::ostringstream ss;
            for (std::size_t i = 0; i < undefined_globals.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << undefined_globals[i];
            }
            throw ToolError(source_name + ": .GLOBAL sem definição: " + ss.str());
        }

        json::Value::object_t root;
        root["format"] = json::Value("wirth-obj-v1");
        root["module"] = json::Value(module_name);
        root["size"] = json::Value(static_cast<long long>(code.size()));
        json::Value::array_t code_arr;
        code_arr.reserve(code.size());
        for (auto byte : code) {
            code_arr.emplace_back(static_cast<long long>(byte));
        }
        root["code"] = json::Value(std::move(code_arr));

        json::Value::object_t symbols_obj;
        for (const auto& [name, value] : symbols) {
            symbols_obj[name] = json::Value(value);
        }
        root["symbols"] = json::Value(std::move(symbols_obj));

        std::sort(globals_export.begin(), globals_export.end());
        globals_export.erase(std::unique(globals_export.begin(), globals_export.end()), globals_export.end());
        json::Value::array_t globals_arr;
        for (const auto& g : globals_export) {
            globals_arr.emplace_back(g);
        }
        root["globals"] = json::Value(std::move(globals_arr));

        std::sort(externs.begin(), externs.end());
        externs.erase(std::unique(externs.begin(), externs.end()), externs.end());
        json::Value::array_t externs_arr;
        for (const auto& e : externs) {
            externs_arr.emplace_back(e);
        }
        root["externs"] = json::Value(std::move(externs_arr));

        root["entry"] = json::Value(entry);

        json::Value::array_t relocs_arr;
        for (const auto& reloc : relocations) {
            json::Value::object_t ro;
            ro["offset"] = json::Value(reloc.offset);
            ro["size"] = json::Value(reloc.size);
            ro["symbol"] = json::Value(reloc.symbol);
            ro["addend"] = json::Value(reloc.addend);
            relocs_arr.emplace_back(std::move(ro));
        }
        root["relocations"] = json::Value(std::move(relocs_arr));

        return json::Value(std::move(root));
    }

    json::Value assemble_file(const fs::path& src, const fs::path& obj_out) {
        auto result = assemble_lines(util::read_lines(src), src.stem().string(), src.string());
        util::write_text(obj_out, json::dump(result));
        return result;
    }

private:
    static const std::map<std::string, long long> kInstrSizes;

    static std::string upper(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        return s;
    }

    static std::pair<std::string, std::string> split_label(const std::string& line) {
        auto pos = line.find(':');
        if (pos == std::string::npos) {
            return {std::string{}, line};
        }
        std::string left = util::trim(line.substr(0, pos));
        std::string right = util::trim(line.substr(pos + 1));
        if (left.empty()) {
            return {std::string{}, line};
        }
        return {left, right};
    }

    static void emit_u16(std::vector<std::uint8_t>& code, long long offset, long long value) {
        code[static_cast<std::size_t>(offset)] = static_cast<std::uint8_t>(value & 0xFF);
        code[static_cast<std::size_t>(offset + 1)] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    }

    static void emit_symbol_u16(
        std::vector<std::uint8_t>& code,
        long long offset,
        const std::string& symbol,
        const std::map<std::string, long long>& symbols,
        const std::vector<std::string>& externs,
        std::vector<Reloc>& relocations,
        const std::string& source_name,
        long long line_no) {
        auto it = symbols.find(symbol);
        if (it != symbols.end()) {
            emit_u16(code, offset, it->second);
            relocations.push_back(Reloc{offset, 2, symbol, 0});
            return;
        }
        if (std::find(externs.begin(), externs.end(), symbol) != externs.end()) {
            emit_u16(code, offset, 0);
            relocations.push_back(Reloc{offset, 2, symbol, 0});
            return;
        }
        throw ToolError(source_name + ":" + std::to_string(line_no) + ": símbolo indefinido " + symbol);
    }

    static long long encode_instruction(
        std::vector<std::uint8_t>& code,
        long long pc,
        const std::string& op,
        const std::vector<std::string>& args,
        const std::map<std::string, long long>& symbols,
        const std::vector<std::string>& externs,
        std::vector<Reloc>& relocations,
        const std::string& source_name,
        long long line_no) {
        static const std::map<std::string, std::uint8_t> opmap = {
            {"NOP", 0x00}, {"HALT", 0x01}, {"RET", 0x02}, {"LOADI", 0x10}, {"LOAD", 0x11}, {"STORE", 0x12},
            {"ADD", 0x20}, {"SUB", 0x21}, {"MUL", 0x22}, {"DIV", 0x23}, {"MOV", 0x24}, {"JMP", 0x30},
            {"JZ", 0x31}, {"JNZ", 0x32}, {"PRINT", 0x40},
        };

        auto it = opmap.find(op);
        if (it == opmap.end()) {
            throw ToolError(source_name + ":" + std::to_string(line_no) + ": instrução não suportada " + op);
        }
        code[static_cast<std::size_t>(pc)] = it->second;
        if (op == "NOP" || op == "HALT" || op == "RET") {
            return pc + 1;
        }
        if (op == "PRINT") {
            if (args.size() != 1) {
                throw ToolError(source_name + ":" + std::to_string(line_no) + ": PRINT exige 1 argumento");
            }
            code[static_cast<std::size_t>(pc + 1)] = static_cast<std::uint8_t>(util::reg_index(args[0]) & 0xFF);
            return pc + 2;
        }
        if (op == "ADD" || op == "SUB" || op == "MUL" || op == "DIV" || op == "MOV") {
            if (args.size() != 2) {
                throw ToolError(source_name + ":" + std::to_string(line_no) + ": " + op + " exige 2 argumentos");
            }
            code[static_cast<std::size_t>(pc + 1)] = static_cast<std::uint8_t>(util::reg_index(args[0]) & 0xFF);
            code[static_cast<std::size_t>(pc + 2)] = static_cast<std::uint8_t>(util::reg_index(args[1]) & 0xFF);
            return pc + 3;
        }
        if (op == "LOADI") {
            if (args.size() != 2) {
                throw ToolError(source_name + ":" + std::to_string(line_no) + ": LOADI exige 2 argumentos");
            }
            code[static_cast<std::size_t>(pc + 1)] = static_cast<std::uint8_t>(util::reg_index(args[0]) & 0xFF);
            long long imm = util::parse_number(args[1]);
            emit_u16(code, pc + 2, imm);
            return pc + 4;
        }
        if (op == "LOAD" || op == "STORE") {
            if (args.size() != 2) {
                throw ToolError(source_name + ":" + std::to_string(line_no) + ": " + op + " exige 2 argumentos");
            }
            code[static_cast<std::size_t>(pc + 1)] = static_cast<std::uint8_t>(util::reg_index(args[0]) & 0xFF);
            const std::string& addr = args[1];
            if (util::is_number(addr)) {
                emit_u16(code, pc + 2, util::parse_number(addr));
            } else {
                emit_symbol_u16(code, pc + 2, addr, symbols, externs, relocations, source_name, line_no);
            }
            return pc + 4;
        }
        if (op == "JMP") {
            if (args.size() != 1) {
                throw ToolError(source_name + ":" + std::to_string(line_no) + ": JMP exige 1 argumento");
            }
            const std::string& target = args[0];
            if (util::is_number(target)) {
                emit_u16(code, pc + 1, util::parse_number(target));
            } else {
                emit_symbol_u16(code, pc + 1, target, symbols, externs, relocations, source_name, line_no);
            }
            return pc + 3;
        }
        if (op == "JZ" || op == "JNZ") {
            if (args.size() != 2) {
                throw ToolError(source_name + ":" + std::to_string(line_no) + ": " + op + " exige 2 argumentos");
            }
            code[static_cast<std::size_t>(pc + 1)] = static_cast<std::uint8_t>(util::reg_index(args[0]) & 0xFF);
            const std::string& target = args[1];
            if (util::is_number(target)) {
                emit_u16(code, pc + 2, util::parse_number(target));
            } else {
                emit_symbol_u16(code, pc + 2, target, symbols, externs, relocations, source_name, line_no);
            }
            return pc + 4;
        }

        throw ToolError(source_name + ":" + std::to_string(line_no) + ": instrução não suportada " + op);
    }
};

const std::map<std::string, long long> TwoPassAssembler::kInstrSizes = {
    {"NOP", 1}, {"HALT", 1}, {"RET", 1}, {"LOADI", 4}, {"LOAD", 4}, {"STORE", 4}, {"ADD", 3},
    {"SUB", 3}, {"MUL", 3}, {"DIV", 3}, {"MOV", 3}, {"JMP", 3}, {"JZ", 4}, {"JNZ", 4}, {"PRINT", 2},
};

struct ModuleView {
    fs::path path;
    ObjectFile data;
    long long base = 0;
};

class TwoPassLinker {
public:
    json::Value link_files(
        const std::vector<fs::path>& obj_paths,
        const fs::path& output_path,
        const std::string& mode,
        long long load_address = 0,
        std::optional<std::string> entry_symbol = std::nullopt) {
        std::vector<ModuleView> modules;
        for (const auto& path : obj_paths) {
            modules.push_back(load_module(path));
        }
        auto image = link_modules(modules, mode, load_address, entry_symbol);
        util::write_text(output_path, json::dump(image));
        return image;
    }

    json::Value link_modules(
        std::vector<ModuleView>& modules,
        const std::string& mode,
        long long load_address = 0,
        std::optional<std::string> entry_symbol = std::nullopt) {
        if (mode != "absolute" && mode != "relocatable") {
            throw ToolError("modo inválido: use absolute ou relocatable");
        }

        long long cursor = 0;
        std::map<std::string, long long> global_symbols;
        for (auto& module : modules) {
            module.base = cursor;
            cursor += module.data.size;
        }

        for (const auto& module : modules) {
            for (const auto& name : module.data.globals) {
                auto def_it = module.data.symbols.find(name);
                if (def_it == module.data.symbols.end()) {
                    throw ToolError(module.path.string() + ": símbolo global sem definição: " + name);
                }
                long long absolute = module.base + def_it->second;
                if (global_symbols.find(name) != global_symbols.end()) {
                    throw ToolError("símbolo global duplicado: " + name);
                }
                global_symbols[name] = absolute;
            }
        }

        std::vector<std::uint8_t> linked(static_cast<std::size_t>(cursor), 0);
        for (const auto& module : modules) {
            std::copy(module.data.code.begin(), module.data.code.end(), linked.begin() + module.base);
        }

        std::vector<json::Value> pending_relocs;
        for (const auto& module : modules) {
            for (const auto& reloc : module.data.relocations) {
                if (reloc.size != 2) {
                    throw ToolError(module.path.string() + ": relocation size não suportado " + std::to_string(reloc.size));
                }
                long long patch_pos = module.base + reloc.offset;
                long long target = resolve_symbol(reloc.symbol, module, global_symbols) + reloc.addend;
                patch_u16(linked, patch_pos, target);
                if (mode == "relocatable") {
                    json::Value::object_t rel;
                    rel["offset"] = json::Value(patch_pos);
                    rel["size"] = json::Value(2);
                    pending_relocs.emplace_back(std::move(rel));
                }
            }
        }

        long long entry = 0;
        if (entry_symbol.has_value()) {
            auto it = global_symbols.find(*entry_symbol);
            if (it == global_symbols.end()) {
                throw ToolError("entry symbol não encontrado: " + *entry_symbol);
            }
            entry = it->second;
        } else if (!modules.empty()) {
            entry = modules[0].base + modules[0].data.entry;
        }

        json::Value::object_t image;
        image["format"] = json::Value("wirth-exe-v1");
        image["mode"] = json::Value(mode);
        image["size"] = json::Value(static_cast<long long>(linked.size()));
        image["entry"] = json::Value(mode == "absolute" ? entry + load_address : entry);
        image["load_address"] = json::Value(mode == "absolute" ? load_address : 0);

        json::Value::array_t code_arr;
        code_arr.reserve(linked.size());
        for (auto byte : linked) {
            code_arr.emplace_back(static_cast<long long>(byte));
        }
        image["code"] = json::Value(std::move(code_arr));

        json::Value::array_t reloc_arr;
        if (mode == "relocatable") {
            reloc_arr = std::move(pending_relocs);
        }
        image["relocations"] = json::Value(std::move(reloc_arr));

        json::Value::object_t symbols_obj;
        for (const auto& [name, value] : global_symbols) {
            symbols_obj[name] = json::Value(value + (mode == "absolute" ? load_address : 0));
        }
        image["symbols"] = json::Value(std::move(symbols_obj));

        return json::Value(std::move(image));
    }

private:
    static ModuleView load_module(const fs::path& path) {
        auto root = json::parse_file(path);
        ObjectFile data = parse_object(root);
        return ModuleView{path, std::move(data), 0};
    }

    static long long resolve_symbol(
        const std::string& symbol,
        const ModuleView& module,
        const std::map<std::string, long long>& global_defs) {
        auto local_it = module.data.symbols.find(symbol);
        if (local_it != module.data.symbols.end()) {
            return module.base + local_it->second;
        }
        auto global_it = global_defs.find(symbol);
        if (global_it != global_defs.end()) {
            return global_it->second;
        }
        throw ToolError(module.path.string() + ": símbolo não resolvido: " + symbol);
    }

    static void patch_u16(std::vector<std::uint8_t>& data, long long offset, long long value) {
        data[static_cast<std::size_t>(offset)] = static_cast<std::uint8_t>(value & 0xFF);
        data[static_cast<std::size_t>(offset + 1)] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    }
};

class Executor {
public:
    struct Result {
        int exit_code = 0;
        long long steps = 0;
        std::vector<long long> registers;
    };

    Result run_file(const fs::path& exe_path, std::optional<long long> load_address = std::nullopt, long long max_steps = 200000) {
        auto data = json::parse_file(exe_path);
        if (!data.is_object() || !data.contains("format") || data.at("format").as_string() != "wirth-exe-v1") {
            throw ToolError("executável inválido: " + exe_path.string());
        }
        return run_image(data.as_object(), load_address, max_steps);
    }

    Result run_image(const json::Value::object_t& image, std::optional<long long> load_address, long long max_steps) {
        std::string mode = image.at("mode").as_string();
        std::vector<std::uint8_t> code;
        for (const auto& item : image.at("code").as_array()) {
            code.push_back(static_cast<std::uint8_t>(item.as_int() & 0xFF));
        }

        long long base = 0;
        if (mode == "absolute") {
            base = image.at("load_address").as_int();
        } else {
            base = load_address.has_value() ? *load_address : image.at("load_address").as_int();
            for (const auto& reloc_value : image.at("relocations").as_array()) {
                const auto& reloc = reloc_value.as_object();
                long long off = reloc.at("offset").as_int();
                long long value = read_u16(code, off);
                patch_u16(code, off, value + base);
            }
        }

        std::vector<std::uint8_t> mem(65536, 0);
        if (base < 0 || base + static_cast<long long>(code.size()) > static_cast<long long>(mem.size())) {
            throw ToolError("imagem fora da memória da VM");
        }
        std::copy(code.begin(), code.end(), mem.begin() + base);

        std::vector<long long> regs(8, 0);
        long long pc = image.at("entry").as_int();
        if (mode == "relocatable") {
            pc += base;
        }
        long long steps = 0;

        while (steps < max_steps) {
            ++steps;
            std::uint8_t op = mem[static_cast<std::size_t>(pc)];
            switch (op) {
                case 0x00: pc += 1; break;
                case 0x01: return Result{0, steps, regs};
                case 0x02: return Result{static_cast<int>(regs[0] & 0xFF), steps, regs};
                case 0x10: {
                    int r = mem[static_cast<std::size_t>(pc + 1)];
                    long long imm = read_u16(mem, pc + 2);
                    regs[static_cast<std::size_t>(r)] = imm;
                    pc += 4;
                    break;
                }
                case 0x11: {
                    int r = mem[static_cast<std::size_t>(pc + 1)];
                    long long addr = read_u16(mem, pc + 2);
                    regs[static_cast<std::size_t>(r)] = read_u16(mem, addr);
                    pc += 4;
                    break;
                }
                case 0x12: {
                    int r = mem[static_cast<std::size_t>(pc + 1)];
                    long long addr = read_u16(mem, pc + 2);
                    patch_u16(mem, addr, regs[static_cast<std::size_t>(r)]);
                    pc += 4;
                    break;
                }
                case 0x20: {
                    int a = mem[static_cast<std::size_t>(pc + 1)];
                    int b = mem[static_cast<std::size_t>(pc + 2)];
                    regs[static_cast<std::size_t>(a)] = (regs[static_cast<std::size_t>(a)] + regs[static_cast<std::size_t>(b)]) & 0xFFFF;
                    pc += 3;
                    break;
                }
                case 0x21: {
                    int a = mem[static_cast<std::size_t>(pc + 1)];
                    int b = mem[static_cast<std::size_t>(pc + 2)];
                    regs[static_cast<std::size_t>(a)] = (regs[static_cast<std::size_t>(a)] - regs[static_cast<std::size_t>(b)]) & 0xFFFF;
                    pc += 3;
                    break;
                }
                case 0x22: {
                    int a = mem[static_cast<std::size_t>(pc + 1)];
                    int b = mem[static_cast<std::size_t>(pc + 2)];
                    regs[static_cast<std::size_t>(a)] = (regs[static_cast<std::size_t>(a)] * regs[static_cast<std::size_t>(b)]) & 0xFFFF;
                    pc += 3;
                    break;
                }
                case 0x23: {
                    int a = mem[static_cast<std::size_t>(pc + 1)];
                    int b = mem[static_cast<std::size_t>(pc + 2)];
                    if (regs[static_cast<std::size_t>(b)] == 0) {
                        throw ToolError("divisão por zero");
                    }
                    regs[static_cast<std::size_t>(a)] = (regs[static_cast<std::size_t>(a)] / regs[static_cast<std::size_t>(b)]) & 0xFFFF;
                    pc += 3;
                    break;
                }
                case 0x24: {
                    int a = mem[static_cast<std::size_t>(pc + 1)];
                    int b = mem[static_cast<std::size_t>(pc + 2)];
                    regs[static_cast<std::size_t>(a)] = regs[static_cast<std::size_t>(b)];
                    pc += 3;
                    break;
                }
                case 0x30:
                    pc = read_u16(mem, pc + 1);
                    break;
                case 0x31: {
                    int r = mem[static_cast<std::size_t>(pc + 1)];
                    long long addr = read_u16(mem, pc + 2);
                    pc = regs[static_cast<std::size_t>(r)] == 0 ? addr : pc + 4;
                    break;
                }
                case 0x32: {
                    int r = mem[static_cast<std::size_t>(pc + 1)];
                    long long addr = read_u16(mem, pc + 2);
                    pc = regs[static_cast<std::size_t>(r)] != 0 ? addr : pc + 4;
                    break;
                }
                case 0x40: {
                    int r = mem[static_cast<std::size_t>(pc + 1)];
                    std::cout << regs[static_cast<std::size_t>(r)] << '\n';
                    pc += 2;
                    break;
                }
                default:
                    throw ToolError("opcode inválido: 0x" + hex_byte(op) + " em pc=0x" + hex_word(pc));
            }
        }

        throw ToolError("limite de passos excedido (" + std::to_string(max_steps) + ")");
    }

private:
    static std::string hex_byte(std::uint8_t value) {
        constexpr char hex[] = "0123456789ABCDEF";
        std::string out = "00";
        out[0] = hex[(value >> 4) & 0xF];
        out[1] = hex[value & 0xF];
        return out;
    }

    static std::string hex_word(long long value) {
        constexpr char hex[] = "0123456789ABCDEF";
        std::string out(4, '0');
        for (int i = 0; i < 4; ++i) {
            int shift = (3 - i) * 4;
            out[static_cast<std::size_t>(i)] = hex[(value >> shift) & 0xF];
        }
        return out;
    }

    static void patch_u16(std::vector<std::uint8_t>& data, long long offset, long long value) {
        data[static_cast<std::size_t>(offset)] = static_cast<std::uint8_t>(value & 0xFF);
        data[static_cast<std::size_t>(offset + 1)] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    }

    static long long read_u16(const std::vector<std::uint8_t>& data, long long offset) {
        return static_cast<long long>(data[static_cast<std::size_t>(offset)]) |
               (static_cast<long long>(data[static_cast<std::size_t>(offset + 1)]) << 8);
    }
};

static void print_result(const Executor::Result& result) {
    std::cout << "exit=" << result.exit_code << " steps=" << result.steps << " regs=[";
    for (std::size_t i = 0; i < result.registers.size(); ++i) {
        if (i > 0) {
            std::cout << ", ";
        }
        std::cout << result.registers[i];
    }
    std::cout << "]\n";
}

static long long parse_cli_number(const std::string& text) {
    return util::parse_number(text);
}

static int run_cli(int argc, char** argv) {
    if (argc < 2) {
        throw ToolError("uso: macro|asm|link|exec|build ...");
    }

    std::string cmd = argv[1];
    MacroProcessor macro;
    TwoPassAssembler assembler;
    TwoPassLinker linker;
    Executor executor;

    if (cmd == "macro") {
        if (argc != 4) {
            throw ToolError("uso: macro <entrada> <saida>");
        }
        macro.process_file(argv[2], argv[3]);
        return 0;
    }
    if (cmd == "asm") {
        if (argc != 4) {
            throw ToolError("uso: asm <entrada> <saida>");
        }
        assembler.assemble_file(argv[2], argv[3]);
        return 0;
    }
    if (cmd == "link") {
        std::vector<fs::path> objects;
        fs::path output;
        std::string mode = "absolute";
        long long load_address = 0;
        std::optional<std::string> entry;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-o" || arg == "--output") {
                if (++i >= argc) throw ToolError("-o/--output exige caminho");
                output = argv[i];
            } else if (arg == "--mode") {
                if (++i >= argc) throw ToolError("--mode exige valor");
                mode = argv[i];
            } else if (arg == "--load-address") {
                if (++i >= argc) throw ToolError("--load-address exige valor");
                load_address = parse_cli_number(argv[i]);
            } else if (arg == "--entry") {
                if (++i >= argc) throw ToolError("--entry exige símbolo");
                entry = std::string(argv[i]);
            } else {
                objects.emplace_back(arg);
            }
        }
        if (output.empty()) {
            throw ToolError("uso: link <objetos...> -o <saida> [--mode absolute|relocatable] [--load-address N] [--entry sym]");
        }
        linker.link_files(objects, output, mode, load_address, entry);
        return 0;
    }
    if (cmd == "exec") {
        if (argc < 3) {
            throw ToolError("uso: exec <entrada> [--load-address N] [--max-steps N]");
        }
        fs::path input = argv[2];
        std::optional<long long> load_address;
        long long max_steps = 200000;
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--load-address") {
                if (++i >= argc) throw ToolError("--load-address exige valor");
                load_address = parse_cli_number(argv[i]);
            } else if (arg == "--max-steps") {
                if (++i >= argc) throw ToolError("--max-steps exige valor");
                max_steps = parse_cli_number(argv[i]);
            } else {
                throw ToolError("argumento desconhecido: " + arg);
            }
        }
        auto result = executor.run_file(input, load_address, max_steps);
        print_result(result);
        return 0;
    }
    if (cmd == "build") {
        if (argc < 3) {
            throw ToolError("uso: build <fontes...> [--out-dir DIR] [--mode absolute|relocatable] [--load-address N] [--entry sym] [--run] [--max-steps N]");
        }
        std::vector<fs::path> sources;
        fs::path out_dir = "build/toolchain";
        std::string mode = "absolute";
        long long load_address = 0;
        std::optional<std::string> entry;
        bool run = false;
        long long max_steps = 200000;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--out-dir") {
                if (++i >= argc) throw ToolError("--out-dir exige diretório");
                out_dir = argv[i];
            } else if (arg == "--mode") {
                if (++i >= argc) throw ToolError("--mode exige valor");
                mode = argv[i];
            } else if (arg == "--load-address") {
                if (++i >= argc) throw ToolError("--load-address exige valor");
                load_address = parse_cli_number(argv[i]);
            } else if (arg == "--entry") {
                if (++i >= argc) throw ToolError("--entry exige símbolo");
                entry = std::string(argv[i]);
            } else if (arg == "--run") {
                run = true;
            } else if (arg == "--max-steps") {
                if (++i >= argc) throw ToolError("--max-steps exige valor");
                max_steps = parse_cli_number(argv[i]);
            } else if (!arg.empty() && arg[0] == '-') {
                throw ToolError("argumento desconhecido: " + arg);
            } else {
                sources.emplace_back(arg);
            }
        }
        if (sources.empty()) {
            throw ToolError("build requer ao menos um arquivo fonte");
        }

        fs::create_directories(out_dir);
        std::vector<fs::path> objects;
        for (const auto& src : sources) {
            fs::path expanded = out_dir / (src.stem().string() + ".mcr.asm");
            fs::path obj = out_dir / (src.stem().string() + ".obj.json");
            macro.process_file(src, expanded);
            assembler.assemble_file(expanded, obj);
            objects.push_back(obj);
        }

        fs::path exe = out_dir / "a.out.json";
        linker.link_files(objects, exe, mode, load_address, entry);
        std::cout << "gerado: " << exe.string() << '\n';
        if (run) {
            auto result = executor.run_file(exe, load_address, max_steps);
            print_result(result);
        }
        return 0;
    }

    throw ToolError("comando desconhecido: " + cmd);
}

int main(int argc, char** argv) {
    try {
        return run_cli(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "erro: " << ex.what() << '\n';
        return 1;
    }
}
