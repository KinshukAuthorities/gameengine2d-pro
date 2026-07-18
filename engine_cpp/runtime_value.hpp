#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace runtime {

template <class T, class = void>
struct is_vector : std::false_type {};
template <class T>
struct is_vector<T, std::void_t<typename T::value_type, decltype(std::declval<T&>().push_back(std::declval<typename T::value_type>()))>> : std::true_type {};

class Value {
public:
    enum class Kind { Null, Bool, Int, Double, String, Object, Array };
    using object_t = std::unordered_map<std::string, Value>;
    using array_t  = std::vector<Value>;

    Value() = default;
    Value(std::nullptr_t) : _kind(Kind::Null) {}
    Value(bool v) : _kind(Kind::Bool), _bool(v) {}
    Value(int v) : _kind(Kind::Int), _int(v) {}
    Value(long v) : _kind(Kind::Int), _int(v) {}
    Value(long long v) : _kind(Kind::Int), _int(v) {}
    Value(unsigned v) : _kind(Kind::Int), _int(static_cast<int64_t>(v)) {}
    Value(unsigned long v) : _kind(Kind::Int), _int(static_cast<int64_t>(v)) {}
    Value(unsigned long long v) : _kind(Kind::Int), _int(static_cast<int64_t>(v)) {}
    Value(float v) : _kind(Kind::Double), _dbl(v) {}
    Value(double v) : _kind(Kind::Double), _dbl(v) {}
    Value(const char* v) : _kind(Kind::String), _str(v ? v : "") {}
    Value(std::string v) : _kind(Kind::String), _str(std::move(v)) {}
    Value(const object_t& v) : _kind(Kind::Object), _obj(std::make_shared<object_t>(v)) {}
    Value(object_t&& v) : _kind(Kind::Object), _obj(std::make_shared<object_t>(std::move(v))) {}
    Value(const array_t& v) : _kind(Kind::Array), _arr(std::make_shared<array_t>(v)) {}
    Value(array_t&& v) : _kind(Kind::Array), _arr(std::make_shared<array_t>(std::move(v))) {}
    Value(std::initializer_list<std::pair<const std::string, Value>> init)
        : _kind(Kind::Object), _obj(std::make_shared<object_t>(init)) {}

    Value(const nlohmann::json& j) { *this = j; }
    Value(nlohmann::json&& j) { *this = std::move(j); }

    static Value object() { Value v; v.ensure_object(); return v; }
    static Value array() { Value v; v.ensure_array(); return v; }

    // Produce a fully independent deep copy of this Value.
    // The default copy constructor/assignment share the underlying shared_ptr,
    // which is correct for cheap transient copies but wrong whenever an entity
    // needs to be truly independent (snapshots, undo, duplicate, instantiate).
    Value deep_clone() const {
        Value out;
        out._kind = _kind;
        out._bool = _bool;
        out._int  = _int;
        out._dbl  = _dbl;
        out._str  = _str;
        if (_obj) {
            out._obj = std::make_shared<object_t>();
            out._obj->reserve(_obj->size());
            for (const auto& [k, v] : *_obj)
                out._obj->emplace(k, v.deep_clone());
        }
        if (_arr) {
            out._arr = std::make_shared<array_t>();
            out._arr->reserve(_arr->size());
            for (const auto& v : *_arr)
                out._arr->push_back(v.deep_clone());
        }
        return out;
    }

    Kind kind() const { return _kind; }
    bool is_null() const { return _kind == Kind::Null; }
    bool is_boolean() const { return _kind == Kind::Bool; }
    bool is_bool() const { return is_boolean(); }
    bool is_number() const { return _kind == Kind::Int || _kind == Kind::Double; }
    bool is_number_integer() const { return _kind == Kind::Int; }
    bool is_number_unsigned() const { return _kind == Kind::Int && _int >= 0; }
    bool is_number_float() const { return _kind == Kind::Double; }
    bool is_string() const { return _kind == Kind::String; }
    bool is_object() const { return _kind == Kind::Object; }
    bool is_array() const { return _kind == Kind::Array; }

    const char* type_name() const {
        switch (_kind) {
            case Kind::Null:   return "null";
            case Kind::Bool:   return "boolean";
            case Kind::Int:    return "integer";
            case Kind::Double: return "number";
            case Kind::String: return "string";
            case Kind::Object: return "object";
            case Kind::Array:  return "array";
        }
        return "null";
    }

    std::size_t size() const {
        if (auto* a = arr_ptr()) return a->size();
        if (auto* o = obj_ptr()) return o->size();
        return 0;
    }

    bool empty() const { return size() == 0; }

    void clear() {
        if (auto* a = arr_ptr()) a->clear();
        else if (auto* o = obj_ptr()) o->clear();
    }

    bool erase(const std::string& key) {
        if (auto* o = obj_ptr()) return o->erase(key) > 0;
        return false;
    }

    // Erase element at index from an array Value. No-op if not an array or out of range.
    bool erase_at(size_t index) {
        if (auto* a = arr_ptr()) {
            if (index < a->size()) {
                a->erase(a->begin() + (ptrdiff_t)index);
                return true;
            }
        }
        return false;
    }

    bool contains(const std::string& key) const {
        if (auto* o = obj_ptr()) return o->find(key) != o->end();
        if (auto* a = arr_ptr()) {
            for (const auto& v : *a) {
                if (v.is_string() && v.get<std::string>() == key) return true;
            }
        }
        return false;
    }

    object_t& items() {
        if (_kind == Kind::Object && _obj) return *_obj;
        static object_t empty;
        return empty;
    }
    const object_t& items() const {
        if (_kind == Kind::Object && _obj) return *_obj;
        static object_t empty;
        return empty;
    }

    Value& operator[](const std::string& key) {
        ensure_object();
        return (*_obj)[key];
    }

    const Value& operator[](const std::string& key) const {
        static Value null_value;
        if (auto* o = obj_ptr()) {
            auto it = o->find(key);
            if (it != o->end()) return it->second;
        }
        return null_value;
    }

    Value& operator[](std::size_t idx) {
        ensure_array();
        if (idx >= _arr->size()) _arr->resize(idx + 1);
        return (*_arr)[idx];
    }

    const Value& operator[](std::size_t idx) const {
        static Value null_value;
        if (auto* a = arr_ptr()) {
            if (idx < a->size()) return (*a)[idx];
        }
        return null_value;
    }

    void push_back(const Value& v) {
        ensure_array();
        _arr->push_back(v);
    }
    void push_back(Value&& v) {
        ensure_array();
        _arr->push_back(std::move(v));
    }

    // Small nlohmann::json-like insert helpers used by editor code.
    Value& insert(std::size_t idx, const Value& v) {
        ensure_array();
        if (idx > _arr->size()) _arr->resize(idx);
        _arr->insert(_arr->begin() + (std::ptrdiff_t)idx, v);
        return (*_arr)[idx];
    }
    Value& insert(std::size_t idx, Value&& v) {
        ensure_array();
        if (idx > _arr->size()) _arr->resize(idx);
        _arr->insert(_arr->begin() + (std::ptrdiff_t)idx, std::move(v));
        return (*_arr)[idx];
    }
    void insert(const std::string& key, const Value& v) {
        ensure_object();
        (*_obj)[key] = v;
    }
    void insert(const std::string& key, Value&& v) {
        ensure_object();
        (*_obj)[key] = std::move(v);
    }

    template <class T>
    T value(const std::string& key, T def) const {
        if (!is_object()) return def;
        const auto& o = *obj_ptr();
        auto it = o.find(key);
        if (it == o.end() || it->second.is_null()) return def;
        return it->second.template get<T>(def);
    }

    std::string value(const std::string& key, const char* def) const {
        if (!is_object()) return def ? def : "";
        const auto& o = *obj_ptr();
        auto it = o.find(key);
        if (it == o.end() || it->second.is_null()) return def ? def : "";
        return it->second.get<std::string>(def ? std::string(def) : std::string{});
    }

    template <class T>
    T get() const {
        return get_impl<T>();
    }

    template <class T>
    T get(const T& def) const {
        if (is_null()) return def;
        return get_impl<T>();
    }

    array_t::iterator begin() { ensure_array(); return _arr->begin(); }
    array_t::iterator end() { ensure_array(); return _arr->end(); }
    array_t::const_iterator begin() const {
        static array_t empty;
        return arr_ptr() ? _arr->cbegin() : empty.cbegin();
    }
    array_t::const_iterator end() const {
        static array_t empty;
        return arr_ptr() ? _arr->cend() : empty.cend();
    }

    Value& operator=(std::nullptr_t) { reset(); return *this; }
    Value& operator=(bool v) { reset_to(Kind::Bool); _bool = v; return *this; }
    Value& operator=(int v) { reset_to(Kind::Int); _int = v; return *this; }
    Value& operator=(long v) { reset_to(Kind::Int); _int = v; return *this; }
    Value& operator=(long long v) { reset_to(Kind::Int); _int = v; return *this; }
    Value& operator=(unsigned v) { reset_to(Kind::Int); _int = static_cast<int64_t>(v); return *this; }
    Value& operator=(unsigned long v) { reset_to(Kind::Int); _int = static_cast<int64_t>(v); return *this; }
    Value& operator=(unsigned long long v) { reset_to(Kind::Int); _int = static_cast<int64_t>(v); return *this; }
    Value& operator=(float v) { reset_to(Kind::Double); _dbl = v; return *this; }
    Value& operator=(double v) { reset_to(Kind::Double); _dbl = v; return *this; }
    Value& operator=(const char* v) { reset_to(Kind::String); _str = v ? v : ""; return *this; }
    Value& operator=(std::string v) { reset_to(Kind::String); _str = std::move(v); return *this; }
    Value& operator=(const object_t& v) { _kind = Kind::Object; _obj = std::make_shared<object_t>(v); clear_primitives(); return *this; }
    Value& operator=(object_t&& v) { _kind = Kind::Object; _obj = std::make_shared<object_t>(std::move(v)); clear_primitives(); return *this; }
    Value& operator=(const array_t& v) { _kind = Kind::Array; _arr = std::make_shared<array_t>(v); clear_primitives(); return *this; }
    Value& operator=(array_t&& v) { _kind = Kind::Array; _arr = std::make_shared<array_t>(std::move(v)); clear_primitives(); return *this; }
    Value& operator=(const std::initializer_list<std::pair<const std::string, Value>>& init) {
        _kind = Kind::Object;
        _obj = std::make_shared<object_t>(init);
        _arr.reset();
        clear_primitives();
        return *this;
    }
    Value& operator=(const nlohmann::json& j) { *this = from_json(j); return *this; }
    Value& operator=(nlohmann::json&& j) { *this = from_json(j); return *this; }

    explicit operator bool() const { return get<bool>(); }
    explicit operator int() const { return get<int>(); }
    explicit operator long() const { return get<long>(); }
    explicit operator long long() const { return get<long long>(); }
    explicit operator unsigned() const { return get<unsigned>(); }
    explicit operator unsigned long() const { return get<unsigned long>(); }
    explicit operator unsigned long long() const { return get<unsigned long long>(); }
    explicit operator float() const { return get<float>(); }
    explicit operator double() const { return get<double>(); }
    explicit operator std::string() const { return get<std::string>(); }
    operator nlohmann::json() const { return to_json(); }

    bool operator<(const Value& rhs) const {
        if (is_number() && rhs.is_number()) return get<double>() < rhs.get<double>();
        if (is_string() && rhs.is_string()) return get<std::string>() < rhs.get<std::string>();
        return static_cast<int>(_kind) < static_cast<int>(rhs._kind);
    }

    Value& ensure(const std::string& key) { return (*this)[key]; }

    friend void to_json(nlohmann::json& j, const Value& v) { j = v.to_json(); }
    friend void from_json(const nlohmann::json& j, Value& v) { v = Value::from_json(j); }


private:
    Kind _kind = Kind::Null;
    bool _bool = false;
    int64_t _int = 0;
    double _dbl = 0.0;
    std::string _str;
    std::shared_ptr<object_t> _obj;
    std::shared_ptr<array_t> _arr;

    void clear_primitives() {
        _bool = false;
        _int = 0;
        _dbl = 0.0;
        _str.clear();
    }

    void reset() {
        _kind = Kind::Null;
        clear_primitives();
        _obj.reset();
        _arr.reset();
    }

    void reset_to(Kind k) {
        _kind = k;
        _obj.reset();
        _arr.reset();
        if (k != Kind::String) _str.clear();
    }

    object_t* obj_ptr() {
        return (_kind == Kind::Object && _obj) ? _obj.get() : nullptr;
    }
    const object_t* obj_ptr() const {
        return (_kind == Kind::Object && _obj) ? _obj.get() : nullptr;
    }
    array_t* arr_ptr() {
        return (_kind == Kind::Array && _arr) ? _arr.get() : nullptr;
    }
    const array_t* arr_ptr() const {
        return (_kind == Kind::Array && _arr) ? _arr.get() : nullptr;
    }

    void ensure_object() {
        if (_kind != Kind::Object || !_obj) {
            _kind = Kind::Object;
            _obj = std::make_shared<object_t>();
            _arr.reset();
            clear_primitives();
        }
    }
    void ensure_array() {
        if (_kind != Kind::Array || !_arr) {
            _kind = Kind::Array;
            _arr = std::make_shared<array_t>();
            _obj.reset();
            clear_primitives();
        }
    }

    static Value from_json(const nlohmann::json& j) {
        try {
            if (j.is_null())             return Value{};
            if (j.is_boolean())          return Value(j.get<bool>());
            if (j.is_number_integer())   return Value(j.get<long long>());
            if (j.is_number_unsigned())  return Value(static_cast<unsigned long long>(j.get<unsigned long long>()));
            if (j.is_number_float())     return Value(j.get<double>());
            if (j.is_string())           return Value(j.get<std::string>());
            if (j.is_array()) {
                array_t a;
                a.reserve(j.size());
                // Index-based — range-for throws type_error.302 when nlohmann's
                // internal type tag is inconsistent (object stored in array slot).
                for (std::size_t i = 0; i < j.size(); ++i) {
                    try { a.push_back(from_json(j.at(i))); }
                    catch (...) { a.push_back(Value{}); }
                }
                return Value(std::move(a));
            }
            if (j.is_object()) {
                object_t o;
                for (auto it = j.begin(); it != j.end(); ++it) {
                    try { o.emplace(it.key(), from_json(it.value())); }
                    catch (...) { o.emplace(it.key(), Value{}); }
                }
                return Value(std::move(o));
            }
            return Value{};
        } catch (...) {
            return Value{};
        }
    }

    nlohmann::json to_json() const {
        switch (_kind) {
            case Kind::Null:
                return nullptr;
            case Kind::Bool:
                return _bool;
            case Kind::Int:
                return _int;
            case Kind::Double:
                return _dbl;
            case Kind::String:
                return _str;
            case Kind::Object: {
                nlohmann::json j = nlohmann::json::object();
                if (_obj) {
                    for (const auto& [k, v] : *_obj) j[k] = v.to_json();
                }
                return j;
            }
            case Kind::Array: {
                nlohmann::json j = nlohmann::json::array();
                if (_arr) {
                    for (const auto& v : *_arr) j.push_back(v.to_json());
                }
                return j;
            }
        }
        return nullptr;
    }

    template <class T>
    T get_impl() const {
        if constexpr (std::is_same_v<T, bool>) {
            if (_kind == Kind::Bool) return _bool;
            if (_kind == Kind::Int) return _int != 0;
            if (_kind == Kind::Double) return std::fabs(_dbl) > 1e-12;
            if (_kind == Kind::String) return !_str.empty();
            return false;
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (_kind == Kind::String) return _str;
            if (_kind == Kind::Int) return std::to_string(_int);
            if (_kind == Kind::Double) return std::to_string(_dbl);
            if (_kind == Kind::Bool) return _bool ? "true" : "false";
            return {};
        } else if constexpr (std::is_integral_v<T>) {
            if (_kind == Kind::Int) return static_cast<T>(_int);
            if (_kind == Kind::Double) return static_cast<T>(_dbl);
            if (_kind == Kind::Bool) return static_cast<T>(_bool ? 1 : 0);
            return T{};
        } else if constexpr (std::is_floating_point_v<T>) {
            if (_kind == Kind::Double) return static_cast<T>(_dbl);
            if (_kind == Kind::Int) return static_cast<T>(_int);
            if (_kind == Kind::Bool) return static_cast<T>(_bool ? 1.0 : 0.0);
            return T{};
        } else if constexpr (std::is_same_v<T, Value>) {
            // Return the Value itself — needed for .value("key", Entity::array())
            // and .value("key", Entity::object()) patterns. Without this branch
            // the compiler falls through to T{} and silently returns an empty/null
            // value regardless of what the field actually contains.
            // Fixes: MainCamera tag detection, PolygonCollider2D, EdgeCollider2D,
            // and EventEmitter payload (§5 of audit report).
            return *this;
        } else if constexpr (runtime::is_vector<T>::value) {
            using U = typename T::value_type;
            T out;
            if (auto* a = arr_ptr()) {
                out.reserve(a->size());
                for (const auto& v : *a) out.push_back(v.template get<U>());
            }
            return out;
        } else {
            return T{};
        }
    }
};

} // namespace runtime