#include "common/json.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <system_error>
#include <type_traits>

namespace zrinput::json {
namespace {

[[noreturn]] void ThrowTypeError(std::string_view expected) {
  throw std::logic_error("JSON value is not " + std::string(expected));
}

int HexValue(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

std::size_t ValidUtf8SequenceLength(std::string_view input,
                                    std::size_t offset) noexcept {
  const auto first = static_cast<unsigned char>(input[offset]);
  std::size_t length = 0;
  std::uint32_t codepoint = 0;
  std::uint32_t minimum = 0;
  if (first >= 0xC2u && first <= 0xDFu) {
    length = 2;
    codepoint = first & 0x1Fu;
    minimum = 0x80u;
  } else if (first >= 0xE0u && first <= 0xEFu) {
    length = 3;
    codepoint = first & 0x0Fu;
    minimum = 0x800u;
  } else if (first >= 0xF0u && first <= 0xF4u) {
    length = 4;
    codepoint = first & 0x07u;
    minimum = 0x10000u;
  } else {
    return 0;
  }
  if (offset + length > input.size()) {
    return 0;
  }
  for (std::size_t index = 1; index < length; ++index) {
    const auto byte = static_cast<unsigned char>(input[offset + index]);
    if ((byte & 0xC0u) != 0x80u) {
      return 0;
    }
    codepoint = (codepoint << 6u) | (byte & 0x3Fu);
  }
  if (codepoint < minimum || codepoint > 0x10FFFFu ||
      (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
    return 0;
  }
  return length;
}

void AppendUtf8(std::uint32_t codepoint, std::string& output) {
  if (codepoint <= 0x7Fu) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FFu) {
    output.push_back(static_cast<char>(0xC0u | (codepoint >> 6u)));
    output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  } else if (codepoint <= 0xFFFFu) {
    output.push_back(static_cast<char>(0xE0u | (codepoint >> 12u)));
    output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
    output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  } else {
    output.push_back(static_cast<char>(0xF0u | (codepoint >> 18u)));
    output.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu)));
    output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
    output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  }
}

class Parser {
 public:
  Parser(std::string_view input, ParseLimits limits)
      : input_(input), limits_(limits) {
    if (input_.size() > limits_.max_input_bytes) {
      Fail("input exceeds byte budget");
    }
    if (limits_.max_depth == 0 || limits_.max_string_bytes == 0 ||
        limits_.max_number_characters == 0 ||
        limits_.max_container_items == 0 || limits_.max_total_values == 0) {
      Fail("parser limits must be non-zero");
    }
  }

  Value ParseDocument() {
    SkipWhitespace();
    if (AtEnd()) {
      Fail("expected a JSON value");
    }
    Value result = ParseValue(0);
    SkipWhitespace();
    if (!AtEnd()) {
      Fail("unexpected trailing data");
    }
    return result;
  }

 private:
  [[noreturn]] void Fail(std::string message) const {
    throw ParseError(offset_, std::move(message));
  }

  [[nodiscard]] bool AtEnd() const noexcept { return offset_ >= input_.size(); }

  [[nodiscard]] char Peek() const {
    if (AtEnd()) {
      Fail("unexpected end of input");
    }
    return input_[offset_];
  }

  bool Consume(char expected) noexcept {
    if (!AtEnd() && input_[offset_] == expected) {
      ++offset_;
      return true;
    }
    return false;
  }

  void SkipWhitespace() noexcept {
    while (!AtEnd()) {
      const char value = input_[offset_];
      if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
        break;
      }
      ++offset_;
    }
  }

  Value ParseValue(std::size_t depth) {
    if (++total_values_ > limits_.max_total_values) {
      Fail("value count exceeds budget");
    }
    switch (Peek()) {
      case 'n':
        ParseLiteral("null");
        return Value(nullptr);
      case 't':
        ParseLiteral("true");
        return Value(true);
      case 'f':
        ParseLiteral("false");
        return Value(false);
      case '"':
        return Value(ParseString());
      case '[':
        return ParseArray(depth);
      case '{':
        return ParseObject(depth);
      default:
        if (Peek() == '-' || (Peek() >= '0' && Peek() <= '9')) {
          return Value(ParseNumber());
        }
        Fail("unexpected token");
    }
  }

  void ParseLiteral(std::string_view literal) {
    if (input_.substr(offset_, literal.size()) != literal) {
      Fail("invalid literal");
    }
    offset_ += literal.size();
  }

  std::uint16_t ParseHexCodeUnit() {
    if (offset_ + 4 > input_.size()) {
      Fail("truncated Unicode escape");
    }
    std::uint16_t result = 0;
    for (std::size_t index = 0; index < 4; ++index) {
      const int digit = HexValue(input_[offset_ + index]);
      if (digit < 0) {
        Fail("invalid Unicode escape");
      }
      result =
          static_cast<std::uint16_t>(static_cast<std::uint16_t>(result << 4u) |
                                     static_cast<std::uint16_t>(digit));
    }
    offset_ += 4;
    return result;
  }

  void CheckStringBudget(const std::string& value) const {
    if (value.size() > limits_.max_string_bytes) {
      Fail("decoded string exceeds byte budget");
    }
  }

  std::string ParseString() {
    if (!Consume('"')) {
      Fail("expected string");
    }
    std::string result;
    while (!AtEnd()) {
      const auto byte = static_cast<unsigned char>(input_[offset_]);
      if (byte == static_cast<unsigned char>('"')) {
        ++offset_;
        return result;
      }
      if (byte == static_cast<unsigned char>('\\')) {
        ++offset_;
        if (AtEnd()) {
          Fail("truncated escape sequence");
        }
        const char escaped = input_[offset_++];
        switch (escaped) {
          case '"':
          case '\\':
          case '/':
            result.push_back(escaped);
            break;
          case 'b':
            result.push_back('\b');
            break;
          case 'f':
            result.push_back('\f');
            break;
          case 'n':
            result.push_back('\n');
            break;
          case 'r':
            result.push_back('\r');
            break;
          case 't':
            result.push_back('\t');
            break;
          case 'u': {
            const std::uint16_t first = ParseHexCodeUnit();
            std::uint32_t codepoint = first;
            if (first >= 0xD800u && first <= 0xDBFFu) {
              if (!Consume('\\') || !Consume('u')) {
                Fail("high surrogate is not followed by a low surrogate");
              }
              const std::uint16_t second = ParseHexCodeUnit();
              if (second < 0xDC00u || second > 0xDFFFu) {
                Fail("invalid low surrogate");
              }
              codepoint =
                  0x10000u +
                  ((static_cast<std::uint32_t>(first) - 0xD800u) << 10u) +
                  (static_cast<std::uint32_t>(second) - 0xDC00u);
            } else if (first >= 0xDC00u && first <= 0xDFFFu) {
              Fail("unpaired low surrogate");
            }
            AppendUtf8(codepoint, result);
            break;
          }
          default:
            Fail("invalid escape sequence");
        }
        CheckStringBudget(result);
        continue;
      }
      if (byte < 0x20u) {
        Fail("unescaped control character in string");
      }
      if (byte < 0x80u) {
        result.push_back(static_cast<char>(byte));
        ++offset_;
      } else {
        const std::size_t length = ValidUtf8SequenceLength(input_, offset_);
        if (length == 0) {
          Fail("invalid UTF-8 in string");
        }
        result.append(input_.substr(offset_, length));
        offset_ += length;
      }
      CheckStringBudget(result);
    }
    Fail("unterminated string");
  }

  double ParseNumber() {
    const std::size_t start = offset_;
    Consume('-');
    if (AtEnd()) {
      Fail("truncated number");
    }
    if (Consume('0')) {
      if (!AtEnd() && input_[offset_] >= '0' && input_[offset_] <= '9') {
        Fail("leading zero in number");
      }
    } else {
      if (Peek() < '1' || Peek() > '9') {
        Fail("invalid integer part");
      }
      while (!AtEnd() && input_[offset_] >= '0' && input_[offset_] <= '9') {
        ++offset_;
      }
    }
    if (Consume('.')) {
      if (AtEnd() || input_[offset_] < '0' || input_[offset_] > '9') {
        Fail("fraction requires at least one digit");
      }
      while (!AtEnd() && input_[offset_] >= '0' && input_[offset_] <= '9') {
        ++offset_;
      }
    }
    if (!AtEnd() && (input_[offset_] == 'e' || input_[offset_] == 'E')) {
      ++offset_;
      if (!AtEnd() && (input_[offset_] == '+' || input_[offset_] == '-')) {
        ++offset_;
      }
      if (AtEnd() || input_[offset_] < '0' || input_[offset_] > '9') {
        Fail("exponent requires at least one digit");
      }
      while (!AtEnd() && input_[offset_] >= '0' && input_[offset_] <= '9') {
        ++offset_;
      }
    }
    const std::size_t length = offset_ - start;
    if (length > limits_.max_number_characters) {
      Fail("number exceeds character budget");
    }
    double result = 0.0;
    const char* begin = input_.data() + start;
    const char* end = begin + length;
    const auto conversion =
        std::from_chars(begin, end, result, std::chars_format::general);
    if (conversion.ec != std::errc{} || conversion.ptr != end ||
        !std::isfinite(result)) {
      Fail("number is not finite or representable");
    }
    return result;
  }

  Value ParseArray(std::size_t depth) {
    if (depth >= limits_.max_depth) {
      Fail("nesting depth exceeds budget");
    }
    Consume('[');
    SkipWhitespace();
    Value::Array result;
    if (Consume(']')) {
      return Value(std::move(result));
    }
    while (true) {
      if (result.size() >= limits_.max_container_items) {
        Fail("array item count exceeds budget");
      }
      result.push_back(ParseValue(depth + 1));
      SkipWhitespace();
      if (Consume(']')) {
        return Value(std::move(result));
      }
      if (!Consume(',')) {
        Fail("expected comma or closing bracket");
      }
      SkipWhitespace();
    }
  }

  Value ParseObject(std::size_t depth) {
    if (depth >= limits_.max_depth) {
      Fail("nesting depth exceeds budget");
    }
    Consume('{');
    SkipWhitespace();
    Value::Object result;
    if (Consume('}')) {
      return Value(std::move(result));
    }
    while (true) {
      if (result.size() >= limits_.max_container_items) {
        Fail("object member count exceeds budget");
      }
      if (Peek() != '"') {
        Fail("object key must be a string");
      }
      std::string key = ParseString();
      SkipWhitespace();
      if (!Consume(':')) {
        Fail("expected colon after object key");
      }
      SkipWhitespace();
      Value value = ParseValue(depth + 1);
      const auto [unused, inserted] =
          result.emplace(std::move(key), std::move(value));
      static_cast<void>(unused);
      if (!inserted) {
        Fail("duplicate object key");
      }
      SkipWhitespace();
      if (Consume('}')) {
        return Value(std::move(result));
      }
      if (!Consume(',')) {
        Fail("expected comma or closing brace");
      }
      SkipWhitespace();
    }
  }

  std::string_view input_;
  ParseLimits limits_;
  std::size_t offset_ = 0;
  std::size_t total_values_ = 0;
};

void AppendIndent(std::string& output, std::size_t depth) {
  output.append(depth * 2, ' ');
}

void AppendEscapedString(std::string_view value, std::string& output) {
  constexpr char kHex[] = "0123456789ABCDEF";
  output.push_back('"');
  for (std::size_t offset = 0; offset < value.size();) {
    const auto byte = static_cast<unsigned char>(value[offset]);
    if (byte >= 0x80u) {
      const std::size_t length = ValidUtf8SequenceLength(value, offset);
      if (length == 0) {
        throw std::invalid_argument("cannot serialize invalid UTF-8");
      }
      output.append(value.substr(offset, length));
      offset += length;
      continue;
    }
    ++offset;
    switch (byte) {
      case '"':
        output.append("\\\"");
        break;
      case '\\':
        output.append("\\\\");
        break;
      case '\b':
        output.append("\\b");
        break;
      case '\f':
        output.append("\\f");
        break;
      case '\n':
        output.append("\\n");
        break;
      case '\r':
        output.append("\\r");
        break;
      case '\t':
        output.append("\\t");
        break;
      default:
        if (byte < 0x20u) {
          output.append("\\u00");
          output.push_back(kHex[(byte >> 4u) & 0x0Fu]);
          output.push_back(kHex[byte & 0x0Fu]);
        } else {
          output.push_back(static_cast<char>(byte));
        }
        break;
    }
  }
  output.push_back('"');
}

void AppendSerialized(const Value& value, bool pretty, std::size_t depth,
                      std::string& output) {
  if (depth > 128) {
    throw std::invalid_argument("JSON value is too deeply nested to serialize");
  }
  if (value.IsNull()) {
    output.append("null");
  } else if (value.IsBool()) {
    output.append(value.AsBool() ? "true" : "false");
  } else if (value.IsNumber()) {
    const double number = value.AsNumber();
    if (!std::isfinite(number)) {
      throw std::invalid_argument("cannot serialize a non-finite number");
    }
    char buffer[64]{};
    const auto conversion = std::to_chars(
        std::begin(buffer), std::end(buffer), number,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (conversion.ec != std::errc{}) {
      throw std::invalid_argument("cannot serialize number");
    }
    output.append(buffer, conversion.ptr);
  } else if (value.IsString()) {
    AppendEscapedString(value.AsString(), output);
  } else if (value.IsArray()) {
    const auto& array = value.AsArray();
    output.push_back('[');
    for (std::size_t index = 0; index < array.size(); ++index) {
      if (index != 0) {
        output.push_back(',');
      }
      if (pretty) {
        output.push_back('\n');
        AppendIndent(output, depth + 1);
      }
      AppendSerialized(array[index], pretty, depth + 1, output);
    }
    if (pretty && !array.empty()) {
      output.push_back('\n');
      AppendIndent(output, depth);
    }
    output.push_back(']');
  } else {
    const auto& object = value.AsObject();
    output.push_back('{');
    std::size_t index = 0;
    for (const auto& [key, item] : object) {
      if (index++ != 0) {
        output.push_back(',');
      }
      if (pretty) {
        output.push_back('\n');
        AppendIndent(output, depth + 1);
      }
      AppendEscapedString(key, output);
      output.append(pretty ? ": " : ":");
      AppendSerialized(item, pretty, depth + 1, output);
    }
    if (pretty && !object.empty()) {
      output.push_back('\n');
      AppendIndent(output, depth);
    }
    output.push_back('}');
  }
}

}  // namespace

ParseError::ParseError(std::size_t offset, std::string message)
    : std::runtime_error("JSON parse error at byte " + std::to_string(offset) +
                         ": " + message),
      offset_(offset) {}

Value::Value() noexcept : storage_(nullptr) {}
Value::Value(std::nullptr_t) noexcept : storage_(nullptr) {}
Value::Value(bool value) noexcept : storage_(value) {}
Value::Value(double value) noexcept : storage_(value) {}
Value::Value(std::string value) : storage_(std::move(value)) {}
Value::Value(const char* value) : storage_(std::string(value)) {}
Value::Value(Array value) : storage_(std::move(value)) {}
Value::Value(Object value) : storage_(std::move(value)) {}

bool Value::IsNull() const noexcept {
  return std::holds_alternative<std::nullptr_t>(storage_);
}
bool Value::IsBool() const noexcept {
  return std::holds_alternative<bool>(storage_);
}
bool Value::IsNumber() const noexcept {
  return std::holds_alternative<double>(storage_);
}
bool Value::IsString() const noexcept {
  return std::holds_alternative<std::string>(storage_);
}
bool Value::IsArray() const noexcept {
  return std::holds_alternative<Array>(storage_);
}
bool Value::IsObject() const noexcept {
  return std::holds_alternative<Object>(storage_);
}

bool Value::AsBool() const {
  if (!IsBool()) {
    ThrowTypeError("a boolean");
  }
  return std::get<bool>(storage_);
}

double Value::AsNumber() const {
  if (!IsNumber()) {
    ThrowTypeError("a number");
  }
  return std::get<double>(storage_);
}

const std::string& Value::AsString() const {
  if (!IsString()) {
    ThrowTypeError("a string");
  }
  return std::get<std::string>(storage_);
}

const Value::Array& Value::AsArray() const {
  if (!IsArray()) {
    ThrowTypeError("an array");
  }
  return std::get<Array>(storage_);
}

const Value::Object& Value::AsObject() const {
  if (!IsObject()) {
    ThrowTypeError("an object");
  }
  return std::get<Object>(storage_);
}

Value::Array& Value::AsArray() {
  if (!IsArray()) {
    ThrowTypeError("an array");
  }
  return std::get<Array>(storage_);
}

Value::Object& Value::AsObject() {
  if (!IsObject()) {
    ThrowTypeError("an object");
  }
  return std::get<Object>(storage_);
}

const Value* Value::Find(std::string_view key) const noexcept {
  if (!IsObject()) {
    return nullptr;
  }
  const auto& object = std::get<Object>(storage_);
  const auto found = object.find(key);
  return found == object.end() ? nullptr : &found->second;
}

Value Parse(std::string_view input, const ParseLimits& limits) {
  return Parser(input, limits).ParseDocument();
}

std::string Serialize(const Value& value, bool pretty) {
  std::string result;
  AppendSerialized(value, pretty, 0, result);
  return result;
}

}  // namespace zrinput::json
