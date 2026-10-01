// Internal helper for reading JSON with good error messages. Not part of the
// public API (it lives in src/, not include/).
#pragma once

#include <array>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cfbsim/data/enums.h"
#include "cfbsim/data/load.h"

namespace cfbsim::detail {

// A Reader is a view of one JSON value plus where it is: the file and the
// path inside the file ("slots.LT.x"). Every accessor checks the type and, on
// a problem, throws a DataError that names the exact location.
class Reader {
  public:
    Reader(const Json& json, std::string file, std::string path = {})
        : json_(&json), file_(std::move(file)), path_(std::move(path)) {}

    const Json& json() const { return *json_; }
    const std::string& file() const { return file_; }
    const std::string& path() const { return path_; }

    // [[noreturn]] tells the compiler this function never returns normally
    // (it always throws), so code after a fail() call is never reached.
    [[noreturn]] void fail(const std::string& message) const;

    // --- structure ---------------------------------------------------------
    bool is_object() const { return json_->is_object(); }
    bool is_array() const { return json_->is_array(); }
    bool is_string() const { return json_->is_string(); }
    bool is_number() const { return json_->is_number(); }
    bool is_bool() const { return json_->is_boolean(); }

    bool has(std::string_view key) const;
    Reader at(std::string_view key) const;                   // required member
    std::optional<Reader> find(std::string_view key) const;  // optional member
    Reader at(std::size_t index) const;                      // array element
    std::size_t size() const;                                // array or object size

    // Fail if the object has any key not in `allowed`. Catches typos such as
    // "progresion" that would otherwise be silently ignored.
    void allow_only(std::initializer_list<std::string_view> allowed) const;

    // Calls f(Reader) for each array element.
    template <typename F>
    void for_each(F&& f) const {
        require_array();
        for (std::size_t i = 0; i < json_->size(); ++i) f(at(i));
    }

    // Calls f(key, Reader) for each object member, in file order.
    template <typename F>
    void for_each_member(F&& f) const {
        require_object();
        for (const auto& [key, value] : json_->items()) {
            f(key, Reader(value, file_, join(key)));
        }
    }

    // --- values ------------------------------------------------------------
    std::string as_string() const;
    double as_number() const;
    int as_int() const;
    bool as_bool() const;
    std::vector<std::string> as_strings() const;

    template <typename E, std::size_t N>
    E as_enum(const std::array<EnumName<E>, N>& table, std::string_view what) const {
        const std::string s = as_string();
        if (auto value = from_name(table, s)) return *value;
        std::string options;
        for (const auto& entry : table) {
            if (!options.empty()) options += ", ";
            options += entry.name;
        }
        fail("unknown " + std::string(what) + " '" + s + "' (expected one of: " + options + ")");
    }

    // Shorthands for members: string("name"), number_or("width", 0.0), ...
    std::string string(std::string_view key) const { return at(key).as_string(); }
    std::string string_or(std::string_view key, std::string fallback) const;
    double number(std::string_view key) const { return at(key).as_number(); }
    double number_or(std::string_view key, double fallback) const;
    int integer(std::string_view key) const { return at(key).as_int(); }
    bool boolean_or(std::string_view key, bool fallback) const;
    std::vector<std::string> strings_or_empty(std::string_view key) const;

    void require_object() const;
    void require_array() const;

  private:
    std::string join(std::string_view key) const {
        return path_.empty() ? std::string(key) : path_ + "." + std::string(key);
    }

    const Json* json_;  // not owned: the Json document outlives every Reader
    std::string file_;
    std::string path_;
};

// Checks "format_version": 1 at the top of a file.
void require_format_version(const Reader& root);

}  // namespace cfbsim::detail
