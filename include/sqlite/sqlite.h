#pragma once
#include <codecvt>
#include <locale>
#include <string>
#include <functional>
#include <stdexcept>
#include <ctime>

//struct sqlite3;
//struct sqlite3_stmt;

#include "sqlite3.h"
#include "utility/function_traits.h"

namespace sqlite {

struct sqlite_exception : public std::runtime_error {
  sqlite_exception(const char* msg) :runtime_error(msg) {}
};

class database;
class database_binder;

template<std::size_t>
class binder;

template<typename T>
database_binder&& operator<<(database_binder&& db, T const&& val);

template<typename T>
void get_col_from_db(database_binder& db, int inx, T& val);

class database_binder {
private:
  sqlite3* const db_;
  std::u16string sql_;
  sqlite3_stmt* stmt_;
  int index_ = 1;

  bool throw_exceptions_ = true;
  bool error_occured_ = false;

  void extract(std::function<void(void)> call_back) {
    int hresult;

    while ((hresult = sqlite3_step(stmt_)) == SQLITE_ROW) {
      call_back();
    }

    if (hresult != SQLITE_DONE) {
      throw_sqlite_error();
    }

    if (sqlite3_finalize(stmt_) != SQLITE_OK) {
      throw_sqlite_error();
    }

    stmt_ = nullptr;
  }

  void extract_single_value(std::function<void(void)> call_back) {
    int hresult;

    if ((hresult = sqlite3_step(stmt_)) == SQLITE_ROW) {
      call_back();
    }

    if ((hresult = sqlite3_step(stmt_)) == SQLITE_ROW) {
      throw_custom_error("not all rows extracted");
    }

    if (hresult != SQLITE_DONE) {
      throw_sqlite_error();
    }

    if (sqlite3_finalize(stmt_) != SQLITE_OK) {
      throw_sqlite_error();
    }

    stmt_ = nullptr;
  }

  void prepare() {
    if (sqlite3_prepare16_v2(db_, sql_.data(), -1, &stmt_, nullptr) != SQLITE_OK) {
      throw_sqlite_error();
    }
  }

  template<typename Type>
  using is_sqlite_value = std::integral_constant<
    bool,
    std::is_floating_point<Type>::value
    || std::is_integral<Type>::value
    || std::is_same<std::string, Type>::value
    || std::is_same<std::u16string, Type>::value
    || std::is_same<sqlite_int64, Type>::value
  >;

  template<typename T>
  friend database_binder&& operator<<(database_binder&& ddb, T const&& val);
  
  template<typename T>
  friend void get_col_from_db(database_binder& ddb, int inx, T& val);

protected:
  database_binder(sqlite3* db, std::u16string const & sql) : db_(db), sql_(sql), stmt_(nullptr) {
    prepare();
  }

  database_binder(sqlite3* db, std::string const & sql) :
    database_binder(db, std::u16string(sql.begin(), sql.end())) { }

public:
  friend class database;

  ~database_binder() {
    throw_exceptions_ = false;
    // Will be executed if no >> operator is found.
    if (stmt_) {
      int hresult;

      while ((hresult = sqlite3_step(stmt_)) == SQLITE_ROW) {}

      if (hresult != SQLITE_DONE) {
        throw_sqlite_error();
      }

      if (sqlite3_finalize(stmt_) != SQLITE_OK) {
        throw_sqlite_error();
      }

      stmt_ = nullptr;
    }
  }

  void throw_sqlite_error() {
    if (throw_exceptions_) {
      throw sqlite_exception(sqlite3_errmsg(db_));
    }
    error_occured_ = true;
  }

  void throw_custom_error(const char* str) {
    if (throw_exceptions_) {
      throw std::runtime_error(str);
    }
    error_occured_ = true;
  }

  template<typename Result>
  typename std::enable_if<is_sqlite_value<Result>::value, void>::type operator>>(
    Result& value) {
    this->extract_single_value([&value, this] {
      get_col_from_db(*this, 0, value);
    });
  }

  template<typename Function>
  typename std::enable_if<!is_sqlite_value<Function>::value, void>::type operator>>(
    Function func) {
    typedef utility::function_traits<Function> traits;

    this->extract([&func, this]() {
      binder<traits::arity>::run(*this, func);
    });
  }
};

class database {
private:
  sqlite3* db_;
  bool connected_;
  bool ownes_db_;

public:
  database(std::u16string const & db_name) :
    db_(nullptr),
    connected_(false),
    ownes_db_(true)
  {
    connected_ = sqlite3_open16(db_name.data(), &db_) == SQLITE_OK;
  }

  database(std::string const & db_name) :
    db_(nullptr),
    connected_(false),
    ownes_db_(true)
  {
#ifdef _MSC_VER
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> conv;
#endif
    connected_ = sqlite3_open16(conv.from_bytes(db_name).data(), &db_) == SQLITE_OK;
  }

  database(sqlite3* db) :
    db_(db),
    connected_(SQLITE_OK),
    ownes_db_(false) { }

  ~database() {
    if (db_ && ownes_db_) {
      sqlite3_close_v2(db_);
      db_ = nullptr;
    }
  }

  database_binder operator<<(std::string const& sql) const {
    return database_binder(db_, sql);
  }
  database_binder operator<<(std::u16string const& sql) const {
    return database_binder(db_, sql);
  }

  operator bool() const {
    return connected_;
  }

  sqlite3_int64 last_insert_rowid() const {
    return sqlite3_last_insert_rowid(db_);
  }
};

template<std::size_t Count>
class binder {
private:
  template<typename Function, std::size_t Index>
  using nth_argument_type = typename utility::function_traits<Function>::template argument<Index>;

public:
  // The `Boundary` needs to be defaulted to `Count` so that the `run` function
  // template is not implicitly instantiated on class template instantiation.
  // Look up section 14.7.1 "Implicit instantiation" of the ISO C++14 Standard
  // and the [dicussion](https://github.com/aminroosta/sqlite_modern_cpp/issues/8)
  // on Github.
  template<typename Function, typename... Values, std::size_t Boundary = Count>
  static typename std::enable_if < (sizeof...(Values) < Boundary), void>::type
  run(database_binder& db, Function& function, Values&&... values) {
    nth_argument_type<Function, sizeof...(Values)> value{};
    get_col_from_db(db, sizeof...(Values), value);
    run<Function>(db, function, std::forward<Values>(values)..., std::move(value));
  }

  template<typename Function, typename... Values, std::size_t Boundary = Count>
  static typename std::enable_if<(sizeof...(Values) == Boundary), void>::type
  run(database_binder&, Function& function, Values&&... values) {
    function(std::move(values)...);
  }
};

// int
template<>
database_binder&& operator<<(database_binder&& db, int const&& val) {
  if (sqlite3_bind_int(db.stmt_, db.index_, val) != SQLITE_OK) {
    db.throw_sqlite_error();
  }
  ++db.index_;
  return std::move(db);
}

template<>
void get_col_from_db(database_binder& db, int inx, int& val) {
  if (sqlite3_column_type(db.stmt_, inx) == SQLITE_NULL) {
    val = 0;
  } else {
    val = sqlite3_column_int(db.stmt_, inx);
  }
}

// sqlite_int64
template<>
database_binder&& operator<<(database_binder&& db, sqlite_int64 const&& val) {
  if (sqlite3_bind_int64(db.stmt_, db.index_, val) != SQLITE_OK) {
    db.throw_sqlite_error();
  }

  ++db.index_;
  return std::move(db);
}

template<>
void get_col_from_db(database_binder& db, int inx, sqlite3_int64& i) {
  if (sqlite3_column_type(db.stmt_, inx) == SQLITE_NULL) {
    i = 0;
  } else {
    i = sqlite3_column_int64(db.stmt_, inx);
  }
}

// float
template<>
database_binder&& operator<<(database_binder&& db, float const&& val) {
  if (sqlite3_bind_double(db.stmt_, db.index_, double(val)) != SQLITE_OK) {
    db.throw_sqlite_error();
  }

  ++db.index_;
  return std::move(db);
}

template<>
void get_col_from_db(database_binder& db, int inx, float& f) {
  if (sqlite3_column_type(db.stmt_, inx) == SQLITE_NULL) {
    f = 0;
  } else {
    f = float(sqlite3_column_double(db.stmt_, inx));
  }
}

// double
template<>
database_binder&& operator<<(database_binder&& db, double const&& val) {
  if (sqlite3_bind_double(db.stmt_, db.index_, val) != SQLITE_OK) {
    db.throw_sqlite_error();
  }

  ++db.index_;
  return std::move(db);
}

template<>
void get_col_from_db(database_binder& db, int inx, double& d) {
  if (sqlite3_column_type(db.stmt_, inx) == SQLITE_NULL) {
    d = 0;
  } else {
    d = sqlite3_column_double(db.stmt_, inx);
  }
}

// std::string
template<>
void get_col_from_db(database_binder& db, int inx, std::string & s) {
  if (sqlite3_column_type(db.stmt_, inx) == SQLITE_NULL) {
    s = std::string();
  } else {
    sqlite3_column_bytes(db.stmt_, inx);
    s = std::string((char*)sqlite3_column_text(db.stmt_, inx));
  }
}

template<>
database_binder&& operator<<(database_binder&& db, std::string const&& txt) {
  if (sqlite3_bind_text(db.stmt_, db.index_, txt.data(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    db.throw_sqlite_error();
  }

  ++db.index_;
  return std::move(db);
}

// std::u16string
template<>
void get_col_from_db(database_binder& db, int inx, std::u16string & w) {
  if (sqlite3_column_type(db.stmt_, inx) == SQLITE_NULL) {
    w = std::u16string();
  } else {
    sqlite3_column_bytes16(db.stmt_, inx);
    w = std::u16string(static_cast<const char16_t*>(sqlite3_column_text16(db.stmt_, inx)));
  }
}

template<>
database_binder&& operator<<(database_binder&& db, std::u16string const&& txt) {
  if (sqlite3_bind_text16(db.stmt_, db.index_, txt.data(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    db.throw_sqlite_error();
  }

  ++db.index_;
  return std::move(db);
}

// Call the rvalue functions.
template<typename T>
database_binder&& operator<<(database_binder&& db, T const& val) { return std::move(db) << std::move(val); }

// Special case for string literals.
template<std::size_t N>
database_binder&& operator<<(database_binder&& db, const char(&STR)[N]) {
  return std::move(db) << std::string(STR);
}

template<std::size_t N>
database_binder&& operator<<(database_binder&& db, const char16_t(&STR)[N]) {
  return std::move(db) << std::u16string(STR);
}

}  // namespace sqlite
