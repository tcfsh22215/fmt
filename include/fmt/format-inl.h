// Formatting library for C++ - implementation
//
// Copyright (c) 2012 - 2016, Victor Zverovich
// All rights reserved.
//
// For the license information refer to format.h.

#ifndef FMT_FORMAT_INL_H_
#define FMT_FORMAT_INL_H_

#ifndef FMT_MODULE
#  include <algorithm>
#  include <cerrno>  // errno
#  include <climits>
#  include <cmath>
#  include <exception>
#endif

#if defined(_WIN32) && !defined(FMT_USE_WRITE_CONSOLE)
#  include <io.h>  // _isatty
#endif

#include "format.h"

#if FMT_USE_LOCALE
#  include <locale>
#endif

#ifndef FMT_FUNC
#  define FMT_FUNC
#endif

FMT_BEGIN_NAMESPACE
namespace detail {

FMT_FUNC void assert_fail(const char* file, int line, const char* message) {
  // Use unchecked std::fprintf to avoid triggering another assertion when
  // writing to stderr fails.
  fprintf(stderr, "%s:%d: assertion failed: %s", file, line, message);
  abort();
}

FMT_FUNC void format_error_code(detail::buffer<char>& out, int error_code,
                                string_view message) noexcept {
  // Report error code making sure that the output fits into
  // inline_buffer_size to avoid dynamic memory allocation and potential
  // bad_alloc.
  out.try_resize(0);
  static const char SEP[] = ": ";
  static const char ERROR_STR[] = "error ";
  // Subtract 2 to account for terminating null characters in SEP and ERROR_STR.
  size_t error_code_size = sizeof(SEP) + sizeof(ERROR_STR) - 2;
  auto abs_value = static_cast<uint32_or_64_or_128_t<int>>(error_code);
  if (detail::is_negative(error_code)) {
    abs_value = 0 - abs_value;
    ++error_code_size;
  }
  error_code_size += detail::to_unsigned(detail::count_digits(abs_value));
  auto it = appender(out);
  if (message.size() <= inline_buffer_size - error_code_size)
    fmt::format_to(it, FMT_STRING("{}{}"), message, SEP);
  fmt::format_to(it, FMT_STRING("{}{}"), ERROR_STR, error_code);
  FMT_ASSERT(out.size() <= inline_buffer_size, "");
}

FMT_FUNC void do_report_error(format_func func, int error_code,
                              const char* message) noexcept {
  memory_buffer full_message;
  func(full_message, error_code, message);
  // Don't use fwrite_all because the latter may throw.
  if (std::fwrite(full_message.data(), full_message.size(), 1, stderr) > 0)
    std::fputc('\n', stderr);
}

// A wrapper around fwrite that throws on error.
inline void fwrite_all(const void* ptr, size_t count, FILE* stream) {
  size_t written = std::fwrite(ptr, 1, count, stream);
  if (written < count)
    FMT_THROW(system_error(errno, FMT_STRING("cannot write to file")));
}

#if FMT_USE_LOCALE
using std::locale;
using std::numpunct;
using std::use_facet;

template <typename Locale>
locale_ref::locale_ref(const Locale& loc) : locale_(&loc) {
  static_assert(std::is_same<Locale, locale>::value, "");
}
#else
struct locale {};
template <typename Char> struct numpunct {
  auto grouping() const -> std::string { return "\03"; }
  auto thousands_sep() const -> Char { return ','; }
  auto decimal_point() const -> Char { return '.'; }
};
template <typename Facet> Facet use_facet(locale) { return {}; }
#endif  // FMT_USE_LOCALE

template <typename Locale> auto locale_ref::get() const -> Locale {
  static_assert(std::is_same<Locale, locale>::value, "");
#if FMT_USE_LOCALE
  if (locale_) return *static_cast<const locale*>(locale_);
#endif
  return locale();
}

template <typename Char>
FMT_FUNC auto thousands_sep_impl(locale_ref loc) -> thousands_sep_result<Char> {
  auto&& facet = use_facet<numpunct<Char>>(loc.get<locale>());
  auto grouping = facet.grouping();
  auto thousands_sep = grouping.empty() ? Char() : facet.thousands_sep();
  return {std::move(grouping), thousands_sep};
}
template <typename Char>
FMT_FUNC auto decimal_point_impl(locale_ref loc) -> Char {
  return use_facet<numpunct<Char>>(loc.get<locale>()).decimal_point();
}

#if FMT_USE_LOCALE
FMT_FUNC auto write_loc(appender out, loc_value value,
                        const format_specs& specs, locale_ref loc) -> bool {
  auto locale = loc.get<std::locale>();
  // We cannot use the num_put<char> facet because it may produce output in
  // a wrong encoding.
  using facet = format_facet<std::locale>;
  if (std::has_facet<facet>(locale))
    return use_facet<facet>(locale).put(out, value, specs);
  return facet(locale).put(out, value, specs);
}
#endif
}  // namespace detail

FMT_FUNC void report_error(const char* message) {
#if FMT_USE_EXCEPTIONS
  // Use FMT_THROW instead of throw to avoid bogus unreachable code warnings
  // from MSVC.
  FMT_THROW(format_error(message));
#else
  fputs(message, stderr);
  abort();
#endif
}

template <typename Locale> typename Locale::id format_facet<Locale>::id;

template <typename Locale> format_facet<Locale>::format_facet(Locale& loc) {
  auto& np = detail::use_facet<detail::numpunct<char>>(loc);
  grouping_ = np.grouping();
  if (!grouping_.empty()) separator_ = std::string(1, np.thousands_sep());
}

#if FMT_USE_LOCALE
template <>
FMT_API FMT_FUNC auto format_facet<std::locale>::do_put(
    appender out, loc_value val, const format_specs& specs) const -> bool {
  return val.visit(
      detail::loc_writer<>{out, specs, separator_, grouping_, decimal_point_});
}
#endif

FMT_FUNC auto vsystem_error(int error_code, string_view fmt, format_args args)
    -> std::system_error {
  auto ec = std::error_code(error_code, std::generic_category());
  return std::system_error(ec, vformat(fmt, args));
}

namespace detail {

template <typename F>
// Compilers should be able to optimize this into the ror instruction.
FMT_CONSTEXPR inline auto rotr(uint32_t n, uint32_t r) noexcept -> uint32_t {
  r &= 31;
  return (n >> r) | (n << (32 - r));
}
FMT_CONSTEXPR inline auto rotr(uint64_t n, uint32_t r) noexcept -> uint64_t {
  r &= 63;
  return (n >> r) | (n << (64 - r));
}

}  // namespace detail

FMT_FUNC detail::utf8_to_utf16::utf8_to_utf16(string_view s) {
  for_each_codepoint(s, [this](uint32_t cp, string_view) {
    if (cp == invalid_code_point) FMT_THROW(std::runtime_error("invalid utf8"));
    if (cp <= 0xFFFF) {
      buffer_.push_back(static_cast<wchar_t>(cp));
    } else {
      cp -= 0x10000;
      buffer_.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
      buffer_.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
    }
    return true;
  });
  buffer_.push_back(0);
}

FMT_FUNC void format_system_error(detail::buffer<char>& out, int error_code,
                                  const char* message) noexcept {
  FMT_TRY {
    auto ec = std::error_code(error_code, std::generic_category());
    detail::write(appender(out), std::system_error(ec, message).what());
    return;
  }
  FMT_CATCH(...) {}
  format_error_code(out, error_code, message);
}

FMT_FUNC void report_system_error(int error_code,
                                  const char* message) noexcept {
  do_report_error(format_system_error, error_code, message);
}

FMT_FUNC auto vformat(string_view fmt, format_args args) -> std::string {
  // Don't optimize the "{}" case to keep the binary size small and because it
  // can be better optimized in fmt::format anyway.
  auto buffer = memory_buffer();
  detail::vformat_to(buffer, fmt, args);
  return to_string(buffer);
}

namespace detail {

FMT_FUNC void vformat_to(buffer<char>& buf, string_view fmt, format_args args,
                         locale_ref loc) {
  auto out = appender(buf);
  if (fmt.size() == 2 && equal2(fmt.data(), "{}"))
    return args.get(0).visit(default_arg_formatter<char>{out});
  parse_format_string(
      fmt, format_handler<char>{parse_context<char>(fmt), {out, args, loc}});
}

template <typename T> struct span {
  T* data;
  size_t size;
};

template <typename F> auto flockfile(F* f) -> decltype(_lock_file(f)) {
  _lock_file(f);
}
template <typename F> auto funlockfile(F* f) -> decltype(_unlock_file(f)) {
  _unlock_file(f);
}

#ifndef getc_unlocked
template <typename F> auto getc_unlocked(F* f) -> decltype(_fgetc_nolock(f)) {
  return _fgetc_nolock(f);
}
#endif

template <typename F = FILE, typename Enable = void>
struct has_flockfile : std::false_type {};

template <typename F>
struct has_flockfile<F, void_t<decltype(flockfile(&std::declval<F&>()))>>
    : std::true_type {};

// A FILE wrapper. F is FILE defined as a template parameter to make system API
// detection work.
template <typename F> class file_base {
 public:
  F* file_;

 public:
  file_base(F* file) : file_(file) {}
  operator F*() const { return file_; }

  // Reads a code unit from the stream.
  auto get() -> int {
    int result = getc_unlocked(file_);
    if (result == EOF && ferror(file_) != 0)
      FMT_THROW(system_error(errno, FMT_STRING("getc failed")));
    return result;
  }

  // Puts the code unit back into the stream buffer.
  void unget(char c) {
    if (ungetc(c, file_) == EOF)
      FMT_THROW(system_error(errno, FMT_STRING("ungetc failed")));
  }

  void flush() { fflush(this->file_); }
};

// A FILE wrapper for glibc.
template <typename F> class glibc_file : public file_base<F> {
 private:
  enum {
    line_buffered = 0x200,  // _IO_LINE_BUF
    unbuffered = 2          // _IO_UNBUFFERED
  };

 public:
  using file_base<F>::file_base;

  auto is_buffered() const -> bool {
    return (this->file_->_flags & unbuffered) == 0;
  }

  void init_buffer() {
    if (this->file_->_IO_write_ptr) return;
    // Force buffer initialization by placing and removing a char in a buffer.
    assume(this->file_->_IO_write_ptr >= this->file_->_IO_write_end);
    putc_unlocked(0, this->file_);
    --this->file_->_IO_write_ptr;
  }

  // Returns the file's read buffer.
  auto get_read_buffer() const -> span<const char> {
    auto ptr = this->file_->_IO_read_ptr;
    return {ptr, to_unsigned(this->file_->_IO_read_end - ptr)};
  }

  // Returns the file's write buffer.
  auto get_write_buffer() const -> span<char> {
    auto ptr = this->file_->_IO_write_ptr;
    return {ptr, to_unsigned(this->file_->_IO_buf_end - ptr)};
  }

  void advance_write_buffer(size_t size) { this->file_->_IO_write_ptr += size; }

  bool needs_flush() const {
    if ((this->file_->_flags & line_buffered) == 0) return false;
    char* end = this->file_->_IO_write_end;
    return memchr(end, '\n', to_unsigned(this->file_->_IO_write_ptr - end));
  }

  void flush() { fflush_unlocked(this->file_); }
};

// A FILE wrapper for Apple's libc.
template <typename F> class apple_file : public file_base<F> {
 private:
  enum {
    line_buffered = 1,  // __SNBF
    unbuffered = 2      // __SLBF
  };

 public:
  using file_base<F>::file_base;

  auto is_buffered() const -> bool {
    return (this->file_->_flags & unbuffered) == 0;
  }

  void init_buffer() {
    if (this->file_->_p) return;
    // Force buffer initialization by placing and removing a char in a buffer.
    putc_unlocked(0, this->file_);
    --this->file_->_p;
    ++this->file_->_w;
  }

  auto get_read_buffer() const -> span<const char> {
    return {reinterpret_cast<char*>(this->file_->_p),
            to_unsigned(this->file_->_r)};
  }

  auto get_write_buffer() const -> span<char> {
    return {reinterpret_cast<char*>(this->file_->_p),
            to_unsigned(this->file_->_bf._base + this->file_->_bf._size -
                        this->file_->_p)};
  }

  void advance_write_buffer(size_t size) {
    this->file_->_p += size;
    this->file_->_w -= size;
  }

  bool needs_flush() const {
    if ((this->file_->_flags & line_buffered) == 0) return false;
    return memchr(this->file_->_p + this->file_->_w, '\n',
                  to_unsigned(-this->file_->_w));
  }
};

// A fallback FILE wrapper.
template <typename F> class fallback_file : public file_base<F> {
 private:
  char next_;  // The next unconsumed character in the buffer.
  bool has_next_ = false;

 public:
  using file_base<F>::file_base;

  auto is_buffered() const -> bool { return false; }
  auto needs_flush() const -> bool { return false; }
  void init_buffer() {}

  auto get_read_buffer() const -> span<const char> {
    return {&next_, has_next_ ? 1u : 0u};
  }

  auto get_write_buffer() const -> span<char> { return {nullptr, 0}; }

  void advance_write_buffer(size_t) {}

  auto get() -> int {
    has_next_ = false;
    return file_base<F>::get();
  }

  void unget(char c) {
    file_base<F>::unget(c);
    next_ = c;
    has_next_ = true;
  }
};

#ifndef FMT_USE_FALLBACK_FILE
#  define FMT_USE_FALLBACK_FILE 0
#endif

template <typename F,
          FMT_ENABLE_IF(sizeof(F::_p) != 0 && !FMT_USE_FALLBACK_FILE)>
auto get_file(F* f, int) -> apple_file<F> {
  return f;
}
template <typename F,
          FMT_ENABLE_IF(sizeof(F::_IO_read_ptr) != 0 && !FMT_USE_FALLBACK_FILE)>
inline auto get_file(F* f, int) -> glibc_file<F> {
  return f;
}

inline auto get_file(FILE* f, ...) -> fallback_file<FILE> { return f; }

using file_ref = decltype(get_file(static_cast<FILE*>(nullptr), 0));

template <typename F = FILE, typename Enable = void>
class file_print_buffer : public buffer<char> {
 public:
  explicit file_print_buffer(F*) : buffer(nullptr, size_t()) {}
};

template <typename F>
class file_print_buffer<F, enable_if_t<has_flockfile<F>::value>>
    : public buffer<char> {
 private:
  file_ref file_;

  static void grow(buffer<char>& base, size_t) {
    auto& self = static_cast<file_print_buffer&>(base);
    self.file_.advance_write_buffer(self.size());
    if (self.file_.get_write_buffer().size == 0) self.file_.flush();
    auto buf = self.file_.get_write_buffer();
    FMT_ASSERT(buf.size > 0, "");
    self.set(buf.data, buf.size);
    self.clear();
  }

 public:
  explicit file_print_buffer(F* f) : buffer(grow, size_t()), file_(f) {
    flockfile(f);
    file_.init_buffer();
    auto buf = file_.get_write_buffer();
    set(buf.data, buf.size);
  }
  ~file_print_buffer() {
    file_.advance_write_buffer(size());
    bool flush = file_.needs_flush();
    F* f = file_;    // Make funlockfile depend on the template parameter F
    funlockfile(f);  // for the system API detection to work.
    if (flush) fflush(file_);
  }
};

#if !defined(_WIN32) || defined(FMT_USE_WRITE_CONSOLE)
FMT_FUNC auto write_console(int, string_view) -> bool { return false; }
#else
using dword = conditional_t<sizeof(long) == 4, unsigned long, unsigned>;
extern "C" __declspec(dllimport) int __stdcall WriteConsoleW(  //
    void*, const void*, dword, dword*, void*);

FMT_FUNC bool write_console(int fd, string_view text) {
  auto u16 = utf8_to_utf16(text);
  return WriteConsoleW(reinterpret_cast<void*>(_get_osfhandle(fd)), u16.c_str(),
                       static_cast<dword>(u16.size()), nullptr, nullptr) != 0;
}
#endif

#ifdef _WIN32
// Print assuming legacy (non-Unicode) encoding.
FMT_FUNC void vprint_mojibake(std::FILE* f, string_view fmt, format_args args,
                              bool newline) {
  auto buffer = memory_buffer();
  detail::vformat_to(buffer, fmt, args);
  if (newline) buffer.push_back('\n');
  fwrite_all(buffer.data(), buffer.size(), f);
}
#endif

FMT_FUNC void print(std::FILE* f, string_view text) {
#if defined(_WIN32) && !defined(FMT_USE_WRITE_CONSOLE)
  int fd = _fileno(f);
  if (_isatty(fd)) {
    std::fflush(f);
    if (write_console(fd, text)) return;
  }
#endif
  fwrite_all(text.data(), text.size(), f);
}
}  // namespace detail

FMT_FUNC void vprint_buffered(std::FILE* f, string_view fmt, format_args args) {
  auto buffer = memory_buffer();
  detail::vformat_to(buffer, fmt, args);
  detail::print(f, {buffer.data(), buffer.size()});
}

FMT_FUNC void vprint(std::FILE* f, string_view fmt, format_args args) {
  if (!detail::file_ref(f).is_buffered() || !detail::has_flockfile<>())
    return vprint_buffered(f, fmt, args);
  auto&& buffer = detail::file_print_buffer<>(f);
  return detail::vformat_to(buffer, fmt, args);
}

FMT_FUNC void vprintln(std::FILE* f, string_view fmt, format_args args) {
  auto buffer = memory_buffer();
  detail::vformat_to(buffer, fmt, args);
  buffer.push_back('\n');
  detail::print(f, {buffer.data(), buffer.size()});
}

FMT_FUNC void vprint(string_view fmt, format_args args) {
  vprint(stdout, fmt, args);
}

namespace detail {

struct singleton {
  unsigned char upper;
  unsigned char lower_count;
};

inline auto is_printable(uint16_t x, const singleton* singletons,
                         size_t singletons_size,
                         const unsigned char* singleton_lowers,
                         const unsigned char* normal, size_t normal_size)
    -> bool {
  auto upper = x >> 8;
  auto lower_start = 0;
  for (size_t i = 0; i < singletons_size; ++i) {
    auto s = singletons[i];
    auto lower_end = lower_start + s.lower_count;
    if (upper < s.upper) break;
    if (upper == s.upper) {
      for (auto j = lower_start; j < lower_end; ++j) {
        if (singleton_lowers[j] == (x & 0xff)) return false;
      }
    }
    lower_start = lower_end;
  }

  auto xsigned = static_cast<int>(x);
  auto current = true;
  for (size_t i = 0; i < normal_size; ++i) {
    auto v = static_cast<int>(normal[i]);
    auto len = (v & 0x80) != 0 ? (v & 0x7f) << 8 | normal[++i] : v;
    xsigned -= len;
    if (xsigned < 0) break;
    current = !current;
  }
  return current;
}

// This code is generated by support/printable.py.
FMT_FUNC auto is_printable(uint32_t cp) -> bool {
  static constexpr singleton singletons0[] = {
      {0x00, 1},  {0x03, 5},  {0x05, 6},  {0x06, 3},  {0x07, 6},  {0x08, 8},
      {0x09, 17}, {0x0a, 28}, {0x0b, 25}, {0x0c, 20}, {0x0d, 16}, {0x0e, 13},
      {0x0f, 4},  {0x10, 3},  {0x12, 18}, {0x13, 9},  {0x16, 1},  {0x17, 5},
      {0x18, 2},  {0x19, 3},  {0x1a, 7},  {0x1c, 2},  {0x1d, 1},  {0x1f, 22},
      {0x20, 3},  {0x2b, 3},  {0x2c, 2},  {0x2d, 11}, {0x2e, 1},  {0x30, 3},
      {0x31, 2},  {0x32, 1},  {0xa7, 2},  {0xa9, 2},  {0xaa, 4},  {0xab, 8},
      {0xfa, 2},  {0xfb, 5},  {0xfd, 4},  {0xfe, 3},  {0xff, 9},
  };
  static constexpr unsigned char singletons0_lower[] = {
      0xad, 0x78, 0x79, 0x8b, 0x8d, 0xa2, 0x30, 0x57, 0x58, 0x8b, 0x8c, 0x90,
      0x1c, 0x1d, 0xdd, 0x0e, 0x0f, 0x4b, 0x4c, 0xfb, 0xfc, 0x2e, 0x2f, 0x3f,
      0x5c, 0x5d, 0x5f, 0xb5, 0xe2, 0x84, 0x8d, 0x8e, 0x91, 0x92, 0xa9, 0xb1,
      0xba, 0xbb, 0xc5, 0xc6, 0xc9, 0xca, 0xde, 0xe4, 0xe5, 0xff, 0x00, 0x04,
      0x11, 0x12, 0x29, 0x31, 0x34, 0x37, 0x3a, 0x3b, 0x3d, 0x49, 0x4a, 0x5d,
      0x84, 0x8e, 0x92, 0xa9, 0xb1, 0xb4, 0xba, 0xbb, 0xc6, 0xca, 0xce, 0xcf,
      0xe4, 0xe5, 0x00, 0x04, 0x0d, 0x0e, 0x11, 0x12, 0x29, 0x31, 0x34, 0x3a,
      0x3b, 0x45, 0x46, 0x49, 0x4a, 0x5e, 0x64, 0x65, 0x84, 0x91, 0x9b, 0x9d,
      0xc9, 0xce, 0xcf, 0x0d, 0x11, 0x29, 0x45, 0x49, 0x57, 0x64, 0x65, 0x8d,
      0x91, 0xa9, 0xb4, 0xba, 0xbb, 0xc5, 0xc9, 0xdf, 0xe4, 0xe5, 0xf0, 0x0d,
      0x11, 0x45, 0x49, 0x64, 0x65, 0x80, 0x84, 0xb2, 0xbc, 0xbe, 0xbf, 0xd5,
      0xd7, 0xf0, 0xf1, 0x83, 0x85, 0x8b, 0xa4, 0xa6, 0xbe, 0xbf, 0xc5, 0xc7,
      0xce, 0xcf, 0xda, 0xdb, 0x48, 0x98, 0xbd, 0xcd, 0xc6, 0xce, 0xcf, 0x49,
      0x4e, 0x4f, 0x57, 0x59, 0x5e, 0x5f, 0x89, 0x8e, 0x8f, 0xb1, 0xb6, 0xb7,
      0xbf, 0xc1, 0xc6, 0xc7, 0xd7, 0x11, 0x16, 0x17, 0x5b, 0x5c, 0xf6, 0xf7,
      0xfe, 0xff, 0x80, 0x0d, 0x6d, 0x71, 0xde, 0xdf, 0x0e, 0x0f, 0x1f, 0x6e,
      0x6f, 0x1c, 0x1d, 0x5f, 0x7d, 0x7e, 0xae, 0xaf, 0xbb, 0xbc, 0xfa, 0x16,
      0x17, 0x1e, 0x1f, 0x46, 0x47, 0x4e, 0x4f, 0x58, 0x5a, 0x5c, 0x5e, 0x7e,
      0x7f, 0xb5, 0xc5, 0xd4, 0xd5, 0xdc, 0xf0, 0xf1, 0xf5, 0x72, 0x73, 0x8f,
      0x74, 0x75, 0x96, 0x2f, 0x5f, 0x26, 0x2e, 0x2f, 0xa7, 0xaf, 0xb7, 0xbf,
      0xc7, 0xcf, 0xd7, 0xdf, 0x9a, 0x40, 0x97, 0x98, 0x30, 0x8f, 0x1f, 0xc0,
      0xc1, 0xce, 0xff, 0x4e, 0x4f, 0x5a, 0x5b, 0x07, 0x08, 0x0f, 0x10, 0x27,
      0x2f, 0xee, 0xef, 0x6e, 0x6f, 0x37, 0x3d, 0x3f, 0x42, 0x45, 0x90, 0x91,
      0xfe, 0xff, 0x53, 0x67, 0x75, 0xc8, 0xc9, 0xd0, 0xd1, 0xd8, 0xd9, 0xe7,
      0xfe, 0xff,
  };
  static constexpr singleton singletons1[] = {
      {0x00, 6},  {0x01, 1}, {0x03, 1},  {0x04, 2}, {0x08, 8},  {0x09, 2},
      {0x0a, 5},  {0x0b, 2}, {0x0e, 4},  {0x10, 1}, {0x11, 2},  {0x12, 5},
      {0x13, 17}, {0x14, 1}, {0x15, 2},  {0x17, 2}, {0x19, 13}, {0x1c, 5},
      {0x1d, 8},  {0x24, 1}, {0x6a, 3},  {0x6b, 2}, {0xbc, 2},  {0xd1, 2},
      {0xd4, 12}, {0xd5, 9}, {0xd6, 2},  {0xd7, 2}, {0xda, 1},  {0xe0, 5},
      {0xe1, 2},  {0xe8, 2}, {0xee, 32}, {0xf0, 4}, {0xf8, 2},  {0xf9, 2},
      {0xfa, 2},  {0xfb, 1},
  };
  static constexpr unsigned char singletons1_lower[] = {
      0x0c, 0x27, 0x3b, 0x3e, 0x4e, 0x4f, 0x8f, 0x9e, 0x9e, 0x9f, 0x06, 0x07,
      0x09, 0x36, 0x3d, 0x3e, 0x56, 0xf3, 0xd0, 0xd1, 0x04, 0x14, 0x18, 0x36,
      0x37, 0x56, 0x57, 0x7f, 0xaa, 0xae, 0xaf, 0xbd, 0x35, 0xe0, 0x12, 0x87,
      0x89, 0x8e, 0x9e, 0x04, 0x0d, 0x0e, 0x11, 0x12, 0x29, 0x31, 0x34, 0x3a,
      0x45, 0x46, 0x49, 0x4a, 0x4e, 0x4f, 0x64, 0x65, 0x5c, 0xb6, 0xb7, 0x1b,
      0x1c, 0x07, 0x08, 0x0a, 0x0b, 0x14, 0x17, 0x36, 0x39, 0x3a, 0xa8, 0xa9,
      0xd8, 0xd9, 0x09, 0x37, 0x90, 0x91, 0xa8, 0x07, 0x0a, 0x3b, 0x3e, 0x66,
      0x69, 0x8f, 0x92, 0x6f, 0x5f, 0xee, 0xef, 0x5a, 0x62, 0x9a, 0x9b, 0x27,
      0x28, 0x55, 0x9d, 0xa0, 0xa1, 0xa3, 0xa4, 0xa7, 0xa8, 0xad, 0xba, 0xbc,
      0xc4, 0x06, 0x0b, 0x0c, 0x15, 0x1d, 0x3a, 0x3f, 0x45, 0x51, 0xa6, 0xa7,
      0xcc, 0xcd, 0xa0, 0x07, 0x19, 0x1a, 0x22, 0x25, 0x3e, 0x3f, 0xc5, 0xc6,
      0x04, 0x20, 0x23, 0x25, 0x26, 0x28, 0x33, 0x38, 0x3a, 0x48, 0x4a, 0x4c,
      0x50, 0x53, 0x55, 0x56, 0x58, 0x5a, 0x5c, 0x5e, 0x60, 0x63, 0x65, 0x66,
      0x6b, 0x73, 0x78, 0x7d, 0x7f, 0x8a, 0xa4, 0xaa, 0xaf, 0xb0, 0xc0, 0xd0,
      0xae, 0xaf, 0x79, 0xcc, 0x6e, 0x6f, 0x93,
  };
  static constexpr unsigned char normal0[] = {
      0x00, 0x20, 0x5f, 0x22, 0x82, 0xdf, 0x04, 0x82, 0x44, 0x08, 0x1b, 0x04,
      0x06, 0x11, 0x81, 0xac, 0x0e, 0x80, 0xab, 0x35, 0x28, 0x0b, 0x80, 0xe0,
      0x03, 0x19, 0x08, 0x01, 0x04, 0x2f, 0x04, 0x34, 0x04, 0x07, 0x03, 0x01,
      0x07, 0x06, 0x07, 0x11, 0x0a, 0x50, 0x0f, 0x12, 0x07, 0x55, 0x07, 0x03,
      0x04, 0x1c, 0x0a, 0x09, 0x03, 0x08, 0x03, 0x07, 0x03, 0x02, 0x03, 0x03,
      0x03, 0x0c, 0x04, 0x05, 0x03, 0x0b, 0x06, 0x01, 0x0e, 0x15, 0x05, 0x3a,
      0x03, 0x11, 0x07, 0x06, 0x05, 0x10, 0x07, 0x57, 0x07, 0x02, 0x07, 0x15,
      0x0d, 0x50, 0x04, 0x43, 0x03, 0x2d, 0x03, 0x01, 0x04, 0x11, 0x06, 0x0f,
      0x0c, 0x3a, 0x04, 0x1d, 0x25, 0x5f, 0x20, 0x6d, 0x04, 0x6a, 0x25, 0x80,
      0xc8, 0x05, 0x82, 0xb0, 0x03, 0x1a, 0x06, 0x82, 0xfd, 0x03, 0x59, 0x07,
      0x15, 0x0b, 0x17, 0x09, 0x14, 0x0c, 0x14, 0x0c, 0x6a, 0x06, 0x0a, 0x06,
      0x1a, 0x06, 0x59, 0x07, 0x2b, 0x05, 0x46, 0x0a, 0x2c, 0x04, 0x0c, 0x04,
      0x01, 0x03, 0x31, 0x0b, 0x2c, 0x04, 0x1a, 0x06, 0x0b, 0x03, 0x80, 0xac,
      0x06, 0x0a, 0x06, 0x21, 0x3f, 0x4c, 0x04, 0x2d, 0x03, 0x74, 0x08, 0x3c,
      0x03, 0x0f, 0x03, 0x3c, 0x07, 0x38, 0x08, 0x2b, 0x05, 0x82, 0xff, 0x11,
      0x18, 0x08, 0x2f, 0x11, 0x2d, 0x03, 0x20, 0x10, 0x21, 0x0f, 0x80, 0x8c,
      0x04, 0x82, 0x97, 0x19, 0x0b, 0x15, 0x88, 0x94, 0x05, 0x2f, 0x05, 0x3b,
      0x07, 0x02, 0x0e, 0x18, 0x09, 0x80, 0xb3, 0x2d, 0x74, 0x0c, 0x80, 0xd6,
      0x1a, 0x0c, 0x05, 0x80, 0xff, 0x05, 0x80, 0xdf, 0x0c, 0xee, 0x0d, 0x03,
      0x84, 0x8d, 0x03, 0x37, 0x09, 0x81, 0x5c, 0x14, 0x80, 0xb8, 0x08, 0x80,
      0xcb, 0x2a, 0x38, 0x03, 0x0a, 0x06, 0x38, 0x08, 0x46, 0x08, 0x0c, 0x06,
      0x74, 0x0b, 0x1e, 0x03, 0x5a, 0x04, 0x59, 0x09, 0x80, 0x83, 0x18, 0x1c,
      0x0a, 0x16, 0x09, 0x4c, 0x04, 0x80, 0x8a, 0x06, 0xab, 0xa4, 0x0c, 0x17,
      0x04, 0x31, 0xa1, 0x04, 0x81, 0xda, 0x26, 0x07, 0x0c, 0x05, 0x05, 0x80,
      0xa5, 0x11, 0x81, 0x6d, 0x10, 0x78, 0x28, 0x2a, 0x06, 0x4c, 0x04, 0x80,
      0x8d, 0x04, 0x80, 0xbe, 0x03, 0x1b, 0x03, 0x0f, 0x0d,
  };
  static constexpr unsigned char normal1[] = {
      0x5e, 0x22, 0x7b, 0x05, 0x03, 0x04, 0x2d, 0x03, 0x66, 0x03, 0x01, 0x2f,
      0x2e, 0x80, 0x82, 0x1d, 0x03, 0x31, 0x0f, 0x1c, 0x04, 0x24, 0x09, 0x1e,
      0x05, 0x2b, 0x05, 0x44, 0x04, 0x0e, 0x2a, 0x80, 0xaa, 0x06, 0x24, 0x04,
      0x24, 0x04, 0x28, 0x08, 0x34, 0x0b, 0x01, 0x80, 0x90, 0x81, 0x37, 0x09,
      0x16, 0x0a, 0x08, 0x80, 0x98, 0x39, 0x03, 0x63, 0x08, 0x09, 0x30, 0x16,
      0x05, 0x21, 0x03, 0x1b, 0x05, 0x01, 0x40, 0x38, 0x04, 0x4b, 0x05, 0x2f,
      0x04, 0x0a, 0x07, 0x09, 0x07, 0x40, 0x20, 0x27, 0x04, 0x0c, 0x09, 0x36,
      0x03, 0x3a, 0x05, 0x1a, 0x07, 0x04, 0x0c, 0x07, 0x50, 0x49, 0x37, 0x33,
      0x0d, 0x33, 0x07, 0x2e, 0x08, 0x0a, 0x81, 0x26, 0x52, 0x4e, 0x28, 0x08,
      0x2a, 0x56, 0x1c, 0x14, 0x17, 0x09, 0x4e, 0x04, 0x1e, 0x0f, 0x43, 0x0e,
      0x19, 0x07, 0x0a, 0x06, 0x48, 0x08, 0x27, 0x09, 0x75, 0x0b, 0x3f, 0x41,
      0x2a, 0x06, 0x3b, 0x05, 0x0a, 0x06, 0x51, 0x06, 0x01, 0x05, 0x10, 0x03,
      0x05, 0x80, 0x8b, 0x62, 0x1e, 0x48, 0x08, 0x0a, 0x80, 0xa6, 0x5e, 0x22,
      0x45, 0x0b, 0x0a, 0x06, 0x0d, 0x13, 0x39, 0x07, 0x0a, 0x36, 0x2c, 0x04,
      0x10, 0x80, 0xc0, 0x3c, 0x64, 0x53, 0x0c, 0x48, 0x09, 0x0a, 0x46, 0x45,
      0x1b, 0x48, 0x08, 0x53, 0x1d, 0x39, 0x81, 0x07, 0x46, 0x0a, 0x1d, 0x03,
      0x47, 0x49, 0x37, 0x03, 0x0e, 0x08, 0x0a, 0x06, 0x39, 0x07, 0x0a, 0x81,
      0x36, 0x19, 0x80, 0xb7, 0x01, 0x0f, 0x32, 0x0d, 0x83, 0x9b, 0x66, 0x75,
      0x0b, 0x80, 0xc4, 0x8a, 0xbc, 0x84, 0x2f, 0x8f, 0xd1, 0x82, 0x47, 0xa1,
      0xb9, 0x82, 0x39, 0x07, 0x2a, 0x04, 0x02, 0x60, 0x26, 0x0a, 0x46, 0x0a,
      0x28, 0x05, 0x13, 0x82, 0xb0, 0x5b, 0x65, 0x4b, 0x04, 0x39, 0x07, 0x11,
      0x40, 0x05, 0x0b, 0x02, 0x0e, 0x97, 0xf8, 0x08, 0x84, 0xd6, 0x2a, 0x09,
      0xa2, 0xf7, 0x81, 0x1f, 0x31, 0x03, 0x11, 0x04, 0x08, 0x81, 0x8c, 0x89,
      0x04, 0x6b, 0x05, 0x0d, 0x03, 0x09, 0x07, 0x10, 0x93, 0x60, 0x80, 0xf6,
      0x0a, 0x73, 0x08, 0x6e, 0x17, 0x46, 0x80, 0x9a, 0x14, 0x0c, 0x57, 0x09,
      0x19, 0x80, 0x87, 0x81, 0x47, 0x03, 0x85, 0x42, 0x0f, 0x15, 0x85, 0x50,
      0x2b, 0x80, 0xd5, 0x2d, 0x03, 0x1a, 0x04, 0x02, 0x81, 0x70, 0x3a, 0x05,
      0x01, 0x85, 0x00, 0x80, 0xd7, 0x29, 0x4c, 0x04, 0x0a, 0x04, 0x02, 0x83,
      0x11, 0x44, 0x4c, 0x3d, 0x80, 0xc2, 0x3c, 0x06, 0x01, 0x04, 0x55, 0x05,
      0x1b, 0x34, 0x02, 0x81, 0x0e, 0x2c, 0x04, 0x64, 0x0c, 0x56, 0x0a, 0x80,
      0xae, 0x38, 0x1d, 0x0d, 0x2c, 0x04, 0x09, 0x07, 0x02, 0x0e, 0x06, 0x80,
      0x9a, 0x83, 0xd8, 0x08, 0x0d, 0x03, 0x0d, 0x03, 0x74, 0x0c, 0x59, 0x07,
      0x0c, 0x14, 0x0c, 0x04, 0x38, 0x08, 0x0a, 0x06, 0x28, 0x08, 0x22, 0x4e,
      0x81, 0x54, 0x0c, 0x15, 0x03, 0x03, 0x05, 0x07, 0x09, 0x19, 0x07, 0x07,
      0x09, 0x03, 0x0d, 0x07, 0x29, 0x80, 0xcb, 0x25, 0x0a, 0x84, 0x06,
  };
  auto lower = static_cast<uint16_t>(cp);
  if (cp < 0x10000) {
    return is_printable(lower, singletons0,
                        sizeof(singletons0) / sizeof(*singletons0),
                        singletons0_lower, normal0, sizeof(normal0));
  }
  if (cp < 0x20000) {
    return is_printable(lower, singletons1,
                        sizeof(singletons1) / sizeof(*singletons1),
                        singletons1_lower, normal1, sizeof(normal1));
  }
  if (0x2a6de <= cp && cp < 0x2a700) return false;
  if (0x2b735 <= cp && cp < 0x2b740) return false;
  if (0x2b81e <= cp && cp < 0x2b820) return false;
  if (0x2cea2 <= cp && cp < 0x2ceb0) return false;
  if (0x2ebe1 <= cp && cp < 0x2f800) return false;
  if (0x2fa1e <= cp && cp < 0x30000) return false;
  if (0x3134b <= cp && cp < 0xe0100) return false;
  if (0xe01f0 <= cp && cp < 0x110000) return false;
  return cp < 0x110000;
}

}  // namespace detail

FMT_END_NAMESPACE

#endif  // FMT_FORMAT_INL_H_
