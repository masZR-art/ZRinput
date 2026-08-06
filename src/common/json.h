#pragma once

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace zrinput::json {

struct ParseLimits {
  std::size_t max_input_bytes = 512 * 1024;
  std::size_t max_depth = 32;
  std::size_t max_string_bytes = 64 * 1024;
  std::size_t max_number_characters = 128;
  std::size_t max_container_items = 4096;
  std::size_t max_total_values = 16384;
};

class ParseError : public std::runtime_error {
 public:
  ParseError(std::size_t offset, std::string message);

  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

 private:
  std::size_t offset_;
};

class Value {
 public:
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value, std::less<>>;

  Value() noexcept;
  Value(std::nullptr_t) noexcept;
  Value(bool value) noexcept;
  Value(double value) noexcept;
  Value(std::string value);
  Value(const char* value);
  Value(Array value);
  Value(Object value);

  [[nodiscard]] bool IsNull() const noexcept;
  [[nodiscard]] bool IsBool() const noexcept;
  [[nodiscard]] bool IsNumber() const noexcept;
  [[nodiscard]] bool IsString() const noexcept;
  [[nodiscard]] bool IsArray() const noexcept;
  [[nodiscard]] bool IsObject() const noexcept;

  [[nodiscard]] bool AsBool() const;
  [[nodiscard]] double AsNumber() const;
  [[nodiscard]] const std::string& AsString() const;
  [[nodiscard]] const Array& AsArray() const;
  [[nodiscard]] const Object& AsObject() const;
  [[nodiscard]] Array& AsArray();
  [[nodiscard]] Object& AsObject();

  [[nodiscard]] const Value* Find(std::string_view key) const noexcept;

  bool operator==(const Value&) const = default;

 private:
  using Storage =
      std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
  Storage storage_;
};

[[nodiscard]] Value Parse(std::string_view input,
                          const ParseLimits& limits = {});
[[nodiscard]] std::string Serialize(const Value& value, bool pretty = false);

}  // namespace zrinput::json
