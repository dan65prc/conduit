
#ifndef CONDUIT_CONDUIT_H_
#define CONDUIT_CONDUIT_H_

#include "botch.h"

#ifndef CONDUIT_NO_LUA
#include "lua-wrapper.h"
#endif
#ifndef CONDUIT_NO_PYTHON
#ifdef _DEBUG
#undef _DEBUG
#include <Python.h>
#define _DEBUG 1
#else
#include <Python.h>
#endif
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>
#include <pybind11/eval.h>
#endif
#include "optional.h"
#include "small-callable.h"
#include "conduit-utility.h"
#include "function.h"
#include <tuple>
#include <set>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <memory>
#include <utility>
#include <initializer_list>
#include <type_traits>
// #define CONDUIT_CHANNEL_TIMES
#ifdef CONDUIT_CHANNEL_TIMES
#include <chrono>
#include <ratio>
#endif

namespace conduit
{
namespace detail
{
    #ifndef CONDUIT_LOGGER
    #define CONDUIT_LOGGER *::conduit::detail::Debug::logger()
    struct Debug
    {
        static std::ostream *&logger()
        {
            static std::ostream *l = &std::cout;
            return l;
        }
    };
    #endif

    struct Names
    {
        #ifdef SOURCE_STRING_INTERNING
        // do not call this directly, use the accessors below
        static std::unordered_map<uint64_t, std::string> &get_names()
        {
            // 0 is always the empty string
            static std::unordered_map<uint64_t, std::string> n{{0, ""}};
            return n;
        }

        static uint64_t get_id_for_string(const std::string &n)
        {
            auto &names = get_names();
            auto i = std::find_if(names.begin(), names.end(), [&n] (auto &p) {
                if (p.second == n)
                    return true;
                return false;
            });
            if (i == names.end()) {
                names[names.size()] = n;
                return names.size() - 1;
            } else {
                return i->first;
            }
        }

        static std::string get_string_for_id(uint64_t id)
        {
            auto &names = get_names();
            if (names.find(id) == names.end())
                return "";
            return names[id];
        }
        #else
        static std::string get_id_for_string(std::string n)
        {
            return n;
        }

        static std::string get_string_for_id(std::string n)
        {
            return n;
        }
        #endif
    };

    #ifdef CONDUIT_CHANNEL_TIMES
    struct Times
    {
        struct Collection
        {
            std::unordered_map<uint64_t, std::chrono::duration<int64_t, std::nano>> times;

            ~Collection()
            {
                std::vector<std::pair<uint64_t, uint64_t>> deco;
                deco.reserve(times.size());
                uint64_t total_time = 0;
                std::for_each(times.begin(), times.end(), [&deco, &total_time] (auto &p) {
                    auto t = std::chrono::duration_cast<std::chrono::microseconds>(p.second).count();
                    total_time += t;
                    deco.push_back(std::make_pair(t, p.first));
                });
                std::sort(deco.begin(), deco.end());
                for (auto &p : deco) {
                    std::cout << Names::get_string_for_id(p.second) << " : " << std::dec << p.first << " : " << ((static_cast<double>(p.first) * 100) / total_time) << "%\n";
                }
            }
        };

        static std::unordered_map<uint64_t, std::chrono::duration<int64_t, std::nano>> &get_times()
        {
            static Collection collection;
            return collection.times;
        }
    };
    #endif

    // print_arg is used to perform default printing, with the exception that
    // user defined types that do not have a print_arg overload will have
    // their type printed.

    template <typename T>
    typename std::enable_if<detail::has_putto_operator<T>::value>::type print_arg(std::ostream &stream, T &&t)
    {
        stream << t;
    }

    template <typename T>
    typename std::enable_if<!detail::has_putto_operator<T>::value>::type print_arg(std::ostream &stream, T &&t)
    {
        stream << demangle(typeid(T).name());
    }

    inline void call_print_arg(std::ostream &) {}

    template <typename T>
    void call_print_arg(std::ostream &stream, T &&t)
    {
        print_arg(stream, std::forward<T>(t));
    }

    template <typename T, typename... U>
    void call_print_arg(std::ostream &stream, T &&t, U &&... u)
    {
        print_arg(stream, std::forward<T>(t));
        stream << ", ";
        call_print_arg(stream, std::forward<U>(u)...);
    }
}

namespace detail
{
    #ifndef CONDUIT_NO_LUA
    using conduit::pop_arg;
    template <typename U, typename... V>
    struct CanConvert {
        template <typename W>
        static char test(decltype(pop_arg(nullptr, 0, (W *)nullptr)) *);
        template <typename W>
        static char *test(...);
        static const bool value = (sizeof(test<U>(nullptr)) == sizeof(char)) && CanConvert<V...>::value;
    };

    template <typename U>
    struct CanConvert<U> {
        template <typename W>
        static char test(decltype(pop_arg(nullptr, 0, (W *)nullptr)) *);
        template <typename W>
        static char *test(...);
        static const bool value = (sizeof(test<U>(nullptr)) == sizeof(char));
    };
    #endif

    struct ExactReturnTypeTag {};
    struct ConvertibleReturnTypeTag {};
    struct OptionalNullTypeTag {};

    template <typename Proposed, typename Actual>
    struct ReturnTypeTag
    {
        using type = typename std::conditional<std::is_convertible<Proposed, Actual>::value, ConvertibleReturnTypeTag, OptionalNullTypeTag>::type;
    };

    template <typename Actual>
    struct ReturnTypeTag<conduit::Optional<Actual>, Actual>
    {
        using type = ExactReturnTypeTag;
    };

    template <typename Actual>
    struct ReturnTypeTag<Actual, Actual>
    {
        using type = ExactReturnTypeTag;
    };
}

// Channel needs to know about Registrars so it can be a friend class.
struct Registrar;
template <typename... T> struct ChannelInterface;
template <typename... T> struct RegistryEntry;
struct ChannelBase {
    virtual ~ChannelBase() {}
};
template <typename ...T> struct Channel;
template <typename R, typename... T>
struct Channel<R(T...)> final : public ChannelBase
{
    virtual ~Channel() {}

    // Never copied, this is to prevent slicing and performance issues.
    // Use a ChannelInterface instead.
    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;

    template <typename R_ = R, typename EnableRet = typename std::enable_if<std::is_same<R_, void>::value>::type>
    void operator()(const T &...t)
    {
        if (callbacks->empty())
            return;

        #ifdef CONDUIT_CHANNEL_TIMES
        auto start = std::chrono::high_resolution_clock::now();
        #endif

        in_callbacks = true;
        for (auto &c : *callbacks) {
            c.cb(t...);
        }
        in_callbacks = false;
        if (pending_unhook.size())
            unhook_();

        #ifdef CONDUIT_CHANNEL_TIMES
        auto end = std::chrono::high_resolution_clock::now();
        auto &t = detail::Times::get_times()[detail::Names::get_id_for_string(name)];
        t += end - start;
        #endif
    }

    template <typename R_ = R, typename EnableRet = typename std::enable_if<!std::is_same<R_, void>::value, R_>::type>
    std::vector<conduit::Optional<R>> operator()(const T &... t)
    {
        if (callbacks->empty())
            return std::vector<conduit::Optional<R>>();

        ret.clear();

        #ifdef CONDUIT_CHANNEL_TIMES
        auto start = std::chrono::high_resolution_clock::now();
        #endif

        in_callbacks = true;
        for (auto &c : *callbacks) {
            ret.emplace_back(c.cb(t...));
        }
        in_callbacks = false;
        if (pending_unhook.size())
            unhook_();

        if (resolves->size()) {
            in_resolves = true;
            for (auto &r : *resolves)
                r.cb(ret);
            in_resolves = false;
            if (pending_unresolve.size())
                unresolve_();
        }

        #ifdef CONDUIT_CHANNEL_TIMES
        auto end = std::chrono::high_resolution_clock::now();
        auto &t = detail::Times::get_times()[detail::Names::get_id_for_string(name)];
        t += end - start;
        #endif
        return ret;
    }

    template <typename C>
    std::string hook(C &&c, std::string client_name, int group = 0)
    {
        BOTCH(in_callbacks, "Can't hook while in_callbacks");
        using C_RET = decltype(c(std::declval<const T>()...));
        hook_(std::forward<C>(c), client_name, group, typename detail::ReturnTypeTag<C_RET, R>::type());
        return client_name;
    }

    void unhook(const std::string &client_name)
    {
        BOTCH(client_name.empty(), "no unhooks of unnamed clients");
        auto pos = std::find_if(callbacks->begin(), callbacks->end(), [client_name] (struct Channel<R(T...)>::Callback &cb) {
            return cb.name == client_name;
        });
        if (pos == callbacks->end())
            return;
        pending_unhook.push_back(std::distance(callbacks->begin(), pos));
        if (!in_callbacks) {
            unhook_();
        }
    }

    void unhook(size_t index)
    {
        if (index < callbacks->size())
            pending_unhook.push_back(index);
        if (!in_callbacks) {
            unhook_();
        }
    }

    template <typename C>
    std::string resolve(C &&c, std::string client_name, int group = 0)
    {
        BOTCH(in_resolves, "Can't resolve while in_resolves");
        auto iter = std::upper_bound(resolves->begin(), resolves->end(), group, [] (int group, const Callback &cb) {
            return group < cb.group;
        });
        resolves->insert(iter, Resolve{c, client_name, group});
        return client_name;
    }

    void unresolve(const std::string &client_name)
    {
        BOTCH(client_name.empty(), "no unhooks of unnamed clients");
        auto pos = std::find_if(resolves->begin(), resolves->end(), [client_name] (struct Channel<R(T...)>::Callback &cb) {
            return cb.name == client_name;
        });
        if (pos == resolves->end())
            return;
        pending_unresolve.push_back(std::distance(resolves->begin(), pos));
        if (!in_callbacks) {
            unhook_();
        }
    }

    void unresolve(size_t index)
    {
        if (index < resolves->size())
            pending_unresolve.push_back(index);
        if (!in_callbacks) {
            unresolve_();
        }
    }

private:
    // Callback definition
    using OperatorReturn = typename std::conditional<std::is_same<R, void>::value, void, std::vector<conduit::Optional<R>>>::type;
    using CallbackReturn = typename std::conditional<std::is_same<R, void>::value, void, conduit::Optional<R>>::type;
    struct Callback
    {
        std::function<CallbackReturn(const T &...)> cb;
        std::string name;
        int group;
    };

    // Resolve definition
    using ResolveFunctionType = typename std::conditional<std::is_same<R, void>::value, void(), void(const std::vector<conduit::Optional<R>>)>::type;
    struct Resolve
    {
        std::function<ResolveFunctionType> cb;
        std::string name;
        int group;
    };

    // data
    std::string name;
    Registrar *registrar = nullptr;
    mutable bool debug = false;
    
    bool in_callbacks = false;
    std::shared_ptr<std::vector<Callback>> callbacks = std::make_shared<std::vector<Callback>>();
    std::vector<size_t> pending_unhook;

    bool in_resolves = false;
    std::shared_ptr<std::vector<Resolve>> resolves = std::make_shared<std::vector<Resolve>>();
    std::vector<size_t> pending_unresolve;

    // Only Registrars can create channels.
    Channel() {}
    friend struct Registrar;
    friend struct RegistryEntry<R(T...)>;
    friend struct ChannelInterface<R(T...)>;

    // No Optional<void>, so pick int
    using RetType = typename std::conditional<std::is_same<R, void>::value, int, conduit::Optional<R>>::type;
    std::vector<RetType> ret;

    template <typename C>
    void hook_(C &&c, const std::string &client_name, int group, detail::ExactReturnTypeTag)
    {
        auto iter = std::upper_bound(callbacks->begin(), callbacks->end(), group, [] (int group, const Callback &cb) {
            return group < cb.group;
        });
        callbacks->insert(iter, Callback{c, client_name, group});
    }

    template <typename C>
    void hook_(C &&c, const std::string &client_name, int group, detail::ConvertibleReturnTypeTag)
    {
        auto capture = c;
        auto iter = std::upper_bound(callbacks->begin(), callbacks->end(), group, [] (int group, const Callback &cb) {
            return group < cb.group;
        });
        callbacks->insert(iter, Callback{[capture] (const T &...t) mutable {return static_cast<R>(capture(t...));}, client_name, group});
    }

    template <typename C>
    void hook_(C &&c, const std::string &client_name, int group, detail::OptionalNullTypeTag)
    {
        auto capture = c;
        auto iter = std::upper_bound(callbacks->begin(), callbacks->end(), group, [] (int group, const Callback &cb) {
            return group < cb.group;
        });
        callbacks->insert(iter, Callback{[capture] (const T &...t) mutable {capture(t...); return conduit::OptionalNull();}, client_name, group});
    }

    void unhook_()
    {
        callbacks->erase(std::remove_if(callbacks->begin(), callbacks->end(), [this] (struct Channel<R(T...)>::Callback &cb) {
            return std::find(pending_unhook.begin(), pending_unhook.end(), &cb - &(*callbacks)[0]) != pending_unhook.end();
        }), callbacks->end());
        pending_unhook.clear();
    }

    void unresolve_()
    {
        resolves->erase(std::remove_if(resolves->begin(), resolves->end(), [this] (struct Channel<R(T...)>::Resolve &cb) {
            return std::find(pending_unresolve.begin(), pending_unresolve.end(), &cb - &(*resolves)[0]) != pending_unresolve.end();
        }), resolves->end());
        pending_unresolve.clear();
    }

    void erase(int index)
    {
        unhook(index);
    }

    // Used by Lua and Python
    static constexpr bool has_arguments = sizeof...(T) > 0;

    void print_debug_impl(const std::string &source, const T &... t);
    void print_debug(const std::string &source, const T &... t)
    {
        if (debug) {
            print_debug_impl(source, t...);
        }
    }

    #ifndef CONDUIT_NO_LUA
    template <std::size_t ...I>
    void call_from_lua_(std::index_sequence<I...>, const std::tuple<T...> &args)
    {
        print_debug("Lua", std::get<I>(args)...);
        this->operator()(std::get<I>(args)...);
    }

    template <int Index>
    struct GetArg
    {
        static typename std::tuple_element<Index, std::tuple<T...>>::type get_arg(lua_State *L)
        {
            using conduit::pop_arg;
            return pop_arg(L, Index + 1, (typename std::tuple_element<Index, std::tuple<T...>>::type *)nullptr);
        }
    };

    template <typename... U, std::size_t ...I>
    typename std::enable_if<detail::CanConvert<U...>::value, bool>::type
    call_from_lua_has_args(lua_State *L, std::index_sequence<I...>)
    {
        if (lua_gettop(L) < static_cast<int>(sizeof...(T))) {
            luaL_error(L, "invalid number of arguments");
            return false;
        }
        std::tuple<T...> args{GetArg<I>::get_arg(L)...};
        call_from_lua_(std::index_sequence_for<T...>{}, args);
        return true;
    }

    template <typename ...U, std::size_t ...I>
    typename std::enable_if<!detail::CanConvert<U...>::value, bool>::type call_from_lua_has_args(lua_State *L, std::index_sequence<I...>)
    {
        using conduit::push_arg;
        lua_settop(L, 0);
        push_arg(L, "Tried to use channel, but there doesn't exist a valid argument conversion");
        return false;
    }

    // Registrar calls a channel from Lua using this interface
    // TODO: add return values back to Lua
    template <bool ha = has_arguments>
    typename std::enable_if<ha, bool>::type call_from_lua(lua_State *L)
    {
        const bool success = call_from_lua_has_args<T...>(L, std::index_sequence_for<T...>{});
        if (success)
            lua_pop(L, (int)sizeof...(T));
        return success;
    }

    template <bool ha = has_arguments>
    typename std::enable_if<!ha, bool>::type call_from_lua(lua_State *L)
    {
        this->operator()();
        return true;
    }
    #endif

    #ifndef CONDUIT_NO_PYTHON
    template <std::size_t ...I>
    void call_from_python_args(const std::string &source, pybind11::args args, std::index_sequence<I...>)
    {
        std::tuple<T...> tuple_args{args[I].cast<T>()...};
        print_debug(source, std::get<I>(tuple_args)...);
        this->operator()(std::get<I>(tuple_args)...);
    }

    template <bool ha = has_arguments>
    typename std::enable_if<ha, bool>::type call_from_python(const std::string &source, pybind11::args args)
    {
        call_from_python_args(source, args, std::index_sequence_for<T...>{});
        return true;
    }

    template <bool ha = has_arguments>
    typename std::enable_if<!ha, bool>::type call_from_python(const std::string &source, pybind11::args args)
    {
        if (args.size())
            throw std::invalid_argument("calling nullary function with arguments");
        print_debug(source);
        this->operator()();
        return true;
    }
    #endif
};

template <typename ...T> struct ChannelInterface;
template <typename R, typename ...T>
struct ChannelInterface<R(T...)>
{
    #ifdef SOURCE_STRING_INTERNING
    uint64_t source_id;
    #else
    std::string source_id;
    #endif
    Channel<R(T...)> *channel;

    // helper for Merge below
    using signature_return_type = R;

    // Make sure we're a POD.
    ChannelInterface() = default;
    ~ChannelInterface() = default;

    typename Channel<R(T...)>::OperatorReturn operator()(const T &...t) const
    {
        auto &c = *channel;
        if (c.debug) {
            CONDUIT_LOGGER << detail::Names::get_string_for_id(source_id) << " -> " << c.registrar->name << "." << channel->name << "(";
            detail::call_print_arg(CONDUIT_LOGGER, t...);
            CONDUIT_LOGGER << ")\n";
        }
        if (c.callbacks->size() == 0)
            return typename Channel<R(T...)>::OperatorReturn();
        return c(t...);
    }

    template <typename C>
    std::string hook(C &&c, const std::string name = "", int group = 0) const
    {
        return channel->hook(std::forward<C>(c), name, group);
    }

    void unhook(std::string client_name)
    {
        channel->unhook(client_name);
    }

    ChannelInterface &set_source_name(const std::string &n)
    {
        source_id = detail::Names::get_id_for_string(n);
        return *this;
    }

    size_t num_callbacks() const
    {
        return channel->callbacks->size();
    }

    std::vector<std::string> callbacks() const
    {
        return std::accumulate(
            channel->callbacks->begin(), channel->callbacks->end(), std::vector<std::string>{},
            [](std::vector<std::string> &vec, struct Channel<R(T...)>::Callback &cb) {
                vec.push_back(cb.name);
                return vec;
            });
    }

    std::string name() const
    {
        return channel->name;
    }

    bool &debug() const
    {
        return channel->debug;
    }

    friend bool operator ==(const ChannelInterface &l, const ChannelInterface &r)
    {
        return l.source_id == r.source_id && l.channel == r.channel;
    }
    friend bool operator !=(const ChannelInterface &l, const ChannelInterface &r)
    {
        return l.source_id != r.source_id || l.channel != r.channel;
    }
};

struct Optuple
{
    virtual void reset() = 0;
    virtual ~Optuple() {}
};

template <typename Callback, typename Data>
struct OptupleImpl : Optuple
{
    detail::TupleState state;
    Data data;
    Callback callback;
    conduit::Function<void()> response;

    template <typename ...T> struct IndexForTuple;
    template <typename ...T> struct IndexForTuple<detail::Tuple<T...>> { using type = std::index_sequence_for<T...>; };

    template <int index> using enabled_type = typename std::enable_if<index != 0>::type;
    template <int, int, typename Enabled, typename ...> struct GenDataIndex;
    template <int index, int count, typename R, typename ...T, typename ...U> struct GenDataIndex<index, count, enabled_type<index>, ChannelInterface<R(T...)>, ChannelInterface<U>...>
    {
        static const int data_index = GenDataIndex<index - 1, count + sizeof...(T), void, ChannelInterface<U>...>::data_index;
    };

    template <int count, typename ...T> struct GenDataIndex<0, count, void, ChannelInterface<T>...>
    {
        static const int data_index = count;
    };

    template <size_t ...I>
    void fire(std::index_sequence<I...>)
    {
        callback(detail::TupleGetVal<I>::get(data)...);
        if (response)
            response();
        reset();
    }

    template <int index>
    int destroy()
    {
        using element_type = typename detail::TupleElement<index, Data>::element_type;
        if (state.val & (1ULL << index)) {
            detail::TupleGetVal<index>::get(data).~element_type();
        }
        return index;
    }

    template <int index, typename V>
    int create(V &&v)
    {
        using element_type = typename detail::TupleElement<index, decltype(data)>::element_type;
        new (&detail::TupleGet<index>::get(data).buf) element_type(std::forward<V>(v));
        return index;
    }

    template <int index, int data_index, uint64_t mask, typename R, typename ...T, size_t ...I>
    int hook(ChannelInterface<R(T...)> ci, std::index_sequence<I...>)
    {
        ci.hook([this] (T ...t) {
            (void) (std::initializer_list<int>{
                destroy<data_index + I>()...
            });
            (void) (std::initializer_list<int>{
                create<data_index + I>(std::move(t))...
            });
            state.val |= (1ULL << index);
            if (state.val == mask) {
                fire(typename IndexForTuple<Data>::type());
            }
        }, "optuple");
        return index;
    }

    template <int index, int data_index, uint64_t mask, typename R, size_t ...I>
    int hook(ChannelInterface<R()> ci, std::index_sequence<I...>)
    {
        ci.hook([this] {
            state.val |= (1ULL << index);
            if (state.val == mask) {
                fire(typename IndexForTuple<Data>::type());
            }
        }, "optuple");
        return index;
    }

    template <size_t ...I, typename ...T>
    void init(std::index_sequence<I...>, ChannelInterface<T> ...cis)
    {
        const uint64_t mask = (1ULL << sizeof...(cis)) - 1;
        (void)(std::initializer_list<int>({
            hook<I, GenDataIndex<I, 0, void, ChannelInterface<T>...>::data_index, mask>(cis, typename CallableInfo<T>::seq_type())...
        }));
    }

    template <size_t ...I>
    void clear(std::index_sequence<I...>)
    {
        (void)(std::initializer_list<int>{
            destroy<I>()...
        });
    }

    void reset() override
    {
        clear(typename IndexForTuple<Data>::type());
        state.val = 0;
    }

    template <typename ...T>
    OptupleImpl(Callback c, ChannelInterface<T> ...cis) : callback(c)
    {
        init(typename std::make_index_sequence<sizeof...(cis)>(), cis...);
    }
};

template <typename ...T> struct TupleConvert;
template <typename ...T> struct TupleConvert<std::tuple<T...>> { using type = detail::Tuple<T...>; };

template <typename C, typename ...T>
std::shared_ptr<Optuple> merge(C &&c, Function<void()> response, ChannelInterface<T> ...cis)
{
    static_assert(sizeof...(cis) <= 64, "optuple supports a maximum of 64 channels");
    using TupleCatType = typename TupleCat<typename CallableInfo<ChannelInterface<T>>::tuple_parameter_type...>::type;
    using Data = typename TupleConvert<TupleCatType>::type;
    using Callback = std::decay_t<C>;
    auto optuple = std::make_shared<OptupleImpl<Callback, Data>>(c, cis...);
    optuple->response = std::move(response);
    return optuple;
}

template <typename C, typename ...T>
std::shared_ptr<Optuple> merge(C &&c, ChannelInterface<T> ...cis)
{
    return merge(c, Function<void()>{}, cis...);
}

// Registry

#ifndef CONDUIT_NO_LUA
template <typename R_, typename... T>
struct LuaChannelBridge
{
    using R = typename std::conditional<detail::CanConvert<R_>::value, R_, void>::type;
    std::shared_ptr<conduit::FunctionWrapper> wrapper;
    LuaChannelBridge(lua_State *L) : wrapper(std::make_shared<conduit::FunctionWrapper>(L)) {}
    R operator()(const T &... t)
    {
        if (wrapper->ref != -1) return wrapper->call<R>(t...);
        return R();
    }
};
#endif

struct RegistryEntryBase {
    std::type_index ti = std::type_index(typeid(void));
    RegistryEntryBase() = default;
    RegistryEntryBase(RegistryEntryBase &&) = default;
    virtual ~RegistryEntryBase(){};
    virtual std::string to_string() const = 0;
    virtual void erase_callback(int) = 0;
    virtual std::vector<std::string> callbacks() const = 0;
    virtual std::string name() const = 0;
    virtual void set_debug(bool) = 0;
    virtual void alias(Registrar &) = 0;

    #ifndef CONDUIT_NO_LUA
    virtual void add_lua_callback(lua_State *, const std::string &, int = 0) = 0;
    virtual bool call_from_lua(lua_State *) = 0;
    #endif
    #ifndef CONDUIT_NO_PYTHON
    virtual void add_python_callback(pybind11::function, const std::string &, int = 0) = 0;
    virtual void call_from_python(const std::string &, pybind11::args) = 0;
    #endif
};

template <typename R, typename... T>
struct RegistryEntry<R(T...)> final : RegistryEntryBase
{
    // can't use make_shared because of private constructor...
    Channel<R(T...)> channel;
    ~RegistryEntry() override {}
    std::string to_string() const override { return demangle(typeid(R(T...)).name()); }
    void erase_callback(int index) override { channel.erase(index); }
    std::vector<std::string> callbacks() const override
    {
        std::vector<std::string> ret;
        std::for_each(channel.callbacks->begin(), channel.callbacks->end(), [&ret] (const auto &cb) {
            ret.push_back(cb.name);
        });
        return ret;
    }
    std::string name() const override {return channel.name;}
    void set_debug(bool debug) override {channel.debug = debug;}
    void alias(Registrar &) override;

    #ifndef CONDUIT_NO_LUA
    void add_lua_callback(lua_State *L, const std::string &client, int group) override
    {
        auto l = LuaChannelBridge<R,T...>(L);
        if (l.wrapper->ref != -1) {
            channel.hook(l, client, group);
        }
    }
    bool call_from_lua(lua_State *L) override { return channel.call_from_lua(L); }
    #endif
    #ifndef CONDUIT_NO_PYTHON
    void add_python_callback(pybind11::function func, const std::string &n, int group) override
    {
        channel.hook(func, n, group);
    }
    void call_from_python(const std::string &source, pybind11::args args) override {channel.call_from_python(source, args);}
    #endif
};

// Registrar is the thing that keeps track of all channels by address.

struct Registrar
{
    std::string name;
    std::unordered_map<std::string, std::unique_ptr<RegistryEntryBase>> map;
    #ifdef CONDUIT_NO_LUA
    using lua_State = void;
    #endif
    lua_State *L;

    Registrar(const std::string &n_, lua_State *L_ = nullptr)
        : name(n_), L(L_)
    {
        #ifndef CONDUIT_NO_PYTHON
        pybind11::module m = pybind11::module::import("__main__");
        pybind11::module conduit = pybind11::reinterpret_borrow<pybind11::module>(PyImport_AddModule("conduit"));
        if (!hasattr(m, "conduit")) {
            setattr(m, "conduit", conduit);
        }
        if (!hasattr(conduit, "registrars")) {
            pybind11::dict reg;
            setattr(conduit, "registrars", reg);
        }
        auto registrars = getattr(conduit, "registrars");
        pybind11::module me(name.c_str(), "conduit");
        registrars[name.c_str()] = me;
        // NOTE: if you get visibility warnings from g++ it's a bug, try adding -fvisibility=hidden
        {
            std::function<pybind11::object(pybind11::str, pybind11::str)> py_lookup = [this] (pybind11::str n_, pybind11::str source_) {
                std::string n = n_;
                if (map.find(n) == map.end()) {
                    throw pybind11::index_error(fmt::format("unable to find \"{}\"\n", n));
                }
                auto &reb = map[n];
                auto obj = pybind11::eval<>("lambda: 0");

                setattr(obj, "name", n_);

                std::string source = source_;
                pybind11::cpp_function call = [&reb, source] (pybind11::args args) {
                    reb->call_from_python(source, args);
                };
                setattr(obj, "call", call);
                pybind11::cpp_function hook{[&reb] (pybind11::function func, pybind11::str n) {
                    reb->add_python_callback(func, n);
                }, pybind11::arg("func"), pybind11::arg("name") = "Python"};
                setattr(obj, "hook", hook);
                pybind11::cpp_function debug = [&reb] (bool debug) { reb->set_debug(debug); };
                setattr(obj, "set_debug", debug);
                pybind11::cpp_function callbacks = [&reb] () {
                    return reb->callbacks();
                };
                setattr(obj, "callbacks", callbacks);
                return obj;
            };
            // moving the cpp_function construction into the setattr call causes
            // g++ 7.X to ICE
            pybind11::cpp_function f(py_lookup, pybind11::arg("channel name"), pybind11::arg("source") = "Python");
            setattr(me, "lookup", f);
            pybind11::cpp_function channels = [this, py_lookup] {
                std::vector<pybind11::object> ret;
                visit([&ret, &py_lookup] (auto &reb) {
                    ret.push_back(py_lookup(reb.name(), std::string("temp")));
                });
                return ret;
            };
            setattr(me, "channels", channels);
            setattr(me, "ptr", pybind11::capsule(this, "ptr"));
            setattr(me, "name", pybind11::str(this->name));
        }
        #endif

        #ifndef CONDUIT_NO_LUA
        using conduit::push_arg;
        if (L) {
            conduit::add_function(L, "conduit.registrars." + name + ".hook", [&]() {
                const int top = lua_gettop(L);
                if (top < 3 || top > 4) {
                    luaL_error(L, "wrong arguments to hook, should be channel name, function, and name of client, and optional group");
                    return;
                }
                std::string channel_name = conduit::pop_arg(L, 1, (std::string *)nullptr);
                std::string client_name = conduit::pop_arg(L, 3, (std::string *)nullptr);
                int group = 0;
                if (top == 4) {
                    group = conduit::pop_arg(L, 4, (int *)nullptr);
                }
                lua_pushvalue(L, 2);
                lua_replace(L, 1);
                lua_settop(L, 1);
                add_lua_callback(channel_name, client_name, group);
                lua_pop(L, 1);
            });
            conduit::add_function(L, "conduit.registrars." + name + ".erase", [&](std::string channel_name, int index) {
                // Lua is 1 based, so calling cl__channel_clients prints a
                // 1-based array. We expect users to use that same index to
                // erase, so we subtract here to restore sanity.
                erase_lua_callback(channel_name, index - 1);
            });
            conduit::add_function(L, "conduit.registrars." + name + ".channels", [=]() {
                lua_newtable(L);
                int i = 1;
                for (auto &p : map) {
                    push_arg(L, p.first);
                    lua_rawseti(L, -2, i++);
                }
            });
            conduit::add_function(L, "conduit.registrars." + name + ".set_debug", [=](const std::string &channel_name, bool debug) {
                if (!map[channel_name]) {
                    lua_pushnil(L);
                    return;
                }
                map[channel_name]->set_debug(debug);
            });
            conduit::add_function(L, "conduit.registrars." + name + ".call", [=]() {
                const int top = lua_gettop(L);
                if (top == 0) {
                    luaL_error(L, "must provide the channel name");
                    return;
                }
                std::string c_name = conduit::pop_arg(L, 1, (std::string *)nullptr);
                // remove the channel name
                lua_remove(L, 1);
                if (!map[c_name]) {
                    luaL_error(L, (std::string("unable to find channel \"") + c_name + "\"").c_str());
                    return;
                }
                if (map[c_name]->call_from_lua(L) == false) {
                    std::string err = conduit::pop_arg(L, -1, (std::string *)nullptr);
                    luaL_error(L, (std::string("error calling channel ") + c_name + ":\n\t" + err).c_str());
                }
            });
        }
        #endif
    }

    ~Registrar()
    {
        #ifndef CONDUIT_NO_PYTHON
        pybind11::eval<>(fmt::format("conduit.registrars.pop('{}', None)\n", name));
        #endif
    }

    template <typename ...U> struct FixType;
    template <typename R, typename ...U> struct FixType<R(U...)> {using type = R(std::decay_t<U>...);};

    template <typename T_>
    ChannelInterface<typename FixType<T_>::type> lookup(const std::string &name, const std::string &source = "")
    {
        using T = typename FixType<T_>::type;
        static_assert(std::is_function<T_>::value, "lookup must be passed a function type");

        auto ti = std::type_index(typeid(T));
        Channel<T> *channel = nullptr;
        if (!map[name]) {
            auto re = std::make_unique<RegistryEntry<T>>();
            re->ti = ti;
            re->channel.name = name;
            re->channel.registrar = this;
            channel = &re->channel;
            map[name] = std::move(re);
        } else {
            auto &re = map[name];
            BOTCH(ti != re->ti, "ERROR: type mismatch for {} (registered {}, requested {})", name, re->to_string(), demangle(typeid(T).name()));
            channel = &reinterpret_cast<RegistryEntry<T> *>(re.get())->channel;
        }
        return ChannelInterface<T>{detail::Names::get_id_for_string(source), channel};
    }

    // NOTE! this operation is not transitive. To alias multiple channels you
    // must use the same base registrar!
    void alias(Registrar &reg, std::string name)
    {
        BOTCH(map.find(name) == map.end(), "alias channel must already exist");
        map[name]->alias(reg);
    }

    void set_debug(bool debug)
    {
        for (auto &p : map) {
            p.second->set_debug(debug);
        }
    }

    template <typename C>
    void visit(C &&c)
    {
        for (auto &p : map) {
            c(*p.second);
        }
    }
    
    #ifndef CONDUIT_NO_LUA
    void add_lua_callback(const std::string &channel_name, const std::string &client_name, int group = 0)
    {
        if (!L)
            return;
        if (!map[channel_name]) {
            std::ostringstream stream;
            stream << "unknown channel " << channel_name;
            luaL_error(L, stream.str().c_str());
            return;
        }
        auto &c = map[channel_name];
        c->add_lua_callback(L, client_name, group);
    }

    void erase_lua_callback(const std::string &channel_name, int index)
    {
        if (!L)
            return;
        if (!map[channel_name]) {
            std::ostringstream stream;
            stream << "unknown channel " << channel_name;
            luaL_error(L, stream.str().c_str());
            return;
        }
        auto &c = map[channel_name];
        c->erase_callback(index);
    }
    #endif
};

template <typename R, typename ...T>
inline void RegistryEntry<R(T...)>::alias(Registrar &reg)
{
    // this ensures the ore exists and agrees on types
    reg.lookup<R(T...)>(channel.name);
    BOTCH(reg.map.find(channel.name) == reg.map.end(), "wha?");
    auto &up = reg.map[channel.name];
    auto &ore = *reinterpret_cast<RegistryEntry<R(T...)> *>(up.get());
    auto &oc = ore.channel;
    std::copy(oc.callbacks->begin(), oc.callbacks->end(), std::back_inserter(*channel.callbacks));
    std::copy(oc.resolves->begin(), oc.resolves->end(), std::back_inserter(*channel.resolves));
    ore.channel.callbacks = channel.callbacks;
    ore.channel.resolves = channel.resolves;
}

template <typename R, typename... T>
void Channel<R(T...)>::print_debug_impl(const std::string &source, const T &... t)
{
    CONDUIT_LOGGER << source << " -> " << registrar->name << "." << name << "(";
    detail::call_print_arg(CONDUIT_LOGGER, t...);
    CONDUIT_LOGGER << ")\n";
}

#define conduit_run_expand_(x, y) x ## y
#define conduit_run_expand(x, y) conduit_run_expand_(x, y)
#define conduit_run(...) std::string conduit_run_expand(conduit_unique_mem_hook_id_, __COUNTER__) = __VA_ARGS__

} // namespace conduit


#endif
