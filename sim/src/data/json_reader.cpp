#include "data/json_reader.h"

#include <algorithm>
#include <fstream>

namespace cfbsim {

DataError::DataError(std::string file, std::string path, const std::string& message)
    : std::runtime_error(file + ": " + (path.empty() ? std::string{} : path + ": ") + message),
      file_(std::move(file)),
      path_(std::move(path)) {}

Json read_json_file(const std::filesystem::path& file, const std::string& source) {
    std::ifstream in(file);
    if (!in) throw DataError(source, "", "cannot open file");
    try {
        // ignore_comments lets data files use // comments.
        return Json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
    } catch (const Json::parse_error& e) {
        throw DataError(source, "", std::string("invalid JSON: ") + e.what());
    }
}

void write_json_file(const std::filesystem::path& file, const Json& json) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    if (!out) throw std::runtime_error("cannot write " + file.string());
    out << json.dump(2) << '\n';
}

namespace detail {

namespace {

std::string type_name(const Json& j) {
    return j.type_name();  // "object", "array", "string", "number", ...
}

}  // namespace

void Reader::fail(const std::string& message) const { throw DataError(file_, path_, message); }

void Reader::require_object() const {
    if (!is_object()) fail("expected an object, got " + type_name(*json_));
}

void Reader::require_array() const {
    if (!is_array()) fail("expected an array, got " + type_name(*json_));
}

bool Reader::has(std::string_view key) const {
    return is_object() && json_->contains(std::string(key));
}

Reader Reader::at(std::string_view key) const {
    require_object();
    const auto it = json_->find(std::string(key));
    if (it == json_->end()) fail("missing required field '" + std::string(key) + "'");
    return Reader(*it, file_, join(key));
}

std::optional<Reader> Reader::find(std::string_view key) const {
    require_object();
    const auto it = json_->find(std::string(key));
    if (it == json_->end()) return std::nullopt;
    return Reader(*it, file_, join(key));
}

Reader Reader::at(std::size_t index) const {
    require_array();
    if (index >= json_->size()) fail("index " + std::to_string(index) + " out of range");
    return Reader((*json_)[index], file_, path_ + "[" + std::to_string(index) + "]");
}

std::size_t Reader::size() const {
    if (!is_array() && !is_object()) fail("expected an array or object");
    return json_->size();
}

void Reader::allow_only(std::initializer_list<std::string_view> allowed) const {
    require_object();
    for (const auto& [key, value] : json_->items()) {
        const bool known = std::find(allowed.begin(), allowed.end(), key) != allowed.end();
        if (!known) {
            Reader(value, file_, join(key)).fail("unknown field '" + key + "'");
        }
    }
}

std::string Reader::as_string() const {
    if (!is_string()) fail("expected a string, got " + type_name(*json_));
    return json_->get<std::string>();
}

double Reader::as_number() const {
    if (!is_number()) fail("expected a number, got " + type_name(*json_));
    return json_->get<double>();
}

int Reader::as_int() const {
    if (!json_->is_number_integer()) fail("expected a whole number, got " + json_->dump());
    return json_->get<int>();
}

bool Reader::as_bool() const {
    if (!is_bool()) fail("expected true or false, got " + type_name(*json_));
    return json_->get<bool>();
}

std::vector<std::string> Reader::as_strings() const {
    std::vector<std::string> out;
    for_each([&](const Reader& item) { out.push_back(item.as_string()); });
    return out;
}

std::string Reader::string_or(std::string_view key, std::string fallback) const {
    if (auto r = find(key)) return r->as_string();
    return fallback;
}

double Reader::number_or(std::string_view key, double fallback) const {
    if (auto r = find(key)) return r->as_number();
    return fallback;
}

bool Reader::boolean_or(std::string_view key, bool fallback) const {
    if (auto r = find(key)) return r->as_bool();
    return fallback;
}

std::vector<std::string> Reader::strings_or_empty(std::string_view key) const {
    if (auto r = find(key)) return r->as_strings();
    return {};
}

void require_format_version(const Reader& root) {
    const int version = root.integer("format_version");
    if (version != 1) {
        root.at("format_version").fail("unsupported format_version " + std::to_string(version));
    }
}

}  // namespace detail
}  // namespace cfbsim
