/*
    Copyright (c) 2011-2013 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2013 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef COCAINE_IO_DEFERRED_SLOT_HPP
#define COCAINE_IO_DEFERRED_SLOT_HPP

#include "cocaine/rpc/slots/function.hpp"

#include <mutex>

#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/get.hpp>
#include <boost/variant/static_visitor.hpp>
#include <boost/variant/variant.hpp>

namespace cocaine {

template<class T> struct deferred;

namespace io {

// Deferred slot

template<class R, class Event>
struct deferred_slot:
    public function_slot<R, Event>
{
    typedef function_slot<R, Event> parent_type;

    typedef typename parent_type::callable_type callable_type;
    typedef typename parent_type::upstream_type upstream_type;

    typedef io::streaming<upstream_type> protocol;

    deferred_slot(callable_type callable):
        parent_type(callable)
    { }

    virtual
    std::shared_ptr<dispatch_t>
    operator()(const msgpack::object& unpacked, const std::shared_ptr<upstream_t>& upstream) {
        typedef deferred<typename result_of<Event>::type> expected_type;

        try {
            // This cast is needed to ensure the correct deferred type.
            (*static_cast<expected_type>(this->call(unpacked)).state).attach(upstream);
        } catch(const std::system_error& e) {
            upstream->send<typename protocol::error>(e.code().value(), std::string(e.code().message()));
            upstream->seal<typename protocol::choke>();
        } catch(const std::exception& e) {
            upstream->send<typename protocol::error>(invocation_error, std::string(e.what()));
            upstream->seal<typename protocol::choke>();
        }

        // Return an empty protocol dispatch.
        return std::shared_ptr<dispatch_t>();
    }
};

namespace aux {

struct unassigned { };

template<class T>
struct value_type {
    value_type(const T& value_): value(value_) { }
    value_type(T&& value_): value(std::move(value_)) { }

    value_type(const value_type& o): value(o.value) { }
    value_type(value_type&& o): value(std::move(o.value)) { }

    value_type&
    operator=(const value_type& rhs) { value = rhs.value; return *this; }

    value_type&
    operator=(value_type&& rhs) { value = std::move(rhs.value); return *this; }

    T value;
};

template<>
struct value_type<void>;

struct error_type {
    error_type(int code_, std::string reason_):
        code(code_),
        reason(reason_)
    { }

    int code;
    std::string reason;
};

struct empty_type { };

template<class T>
struct future_state;

template<class T>
struct future_state_base {
    typedef boost::variant<unassigned, value_type<T>, error_type, empty_type> result_type;
    typedef future_state<T> descendant_type;

    void
    write(T&& value) {
        auto impl = static_cast<descendant_type*>(this);

        std::lock_guard<std::mutex> guard(impl->mutex);

        if(boost::get<unassigned>(&impl->result)) {
            impl->result = value_type<T>(std::forward<T>(value));
            impl->flush();
        }
    }
};

template<>
struct future_state_base<void> {
    typedef boost::variant<unassigned, error_type, empty_type> result_type;
};

template<class T>
struct future_state:
    public future_state_base<T>
{
    friend struct future_state_base<T>;

    void
    abort(int code, const std::string& reason) {
        std::lock_guard<std::mutex> guard(mutex);

        if(boost::get<unassigned>(&result)) {
            result = error_type(code, reason);
            flush();
        }
    }

    void
    close() {
        std::lock_guard<std::mutex> guard(mutex);

        if(boost::get<unassigned>(&result)) {
            result = empty_type();
            flush();
        }
    }

    void
    attach(const std::shared_ptr<upstream_t>& upstream_) {
        std::lock_guard<std::mutex> guard(mutex);

        upstream = upstream_;

        if(!boost::get<unassigned>(&result)) {
            flush();
        }
    }

private:
    void
    flush() {
        if(upstream) boost::apply_visitor(result_visitor_t(upstream), result);
    }

private:
    typename future_state_base<T>::result_type result;

    struct result_visitor_t:
        public boost::static_visitor<void>
    {
        result_visitor_t(const std::shared_ptr<upstream_t>& upstream_):
            upstream(upstream_)
        { }

        void
        operator()(const unassigned&) const {
            // Empty.
        }

        void
        operator()(const value_type<T>& result) const {
            upstream->send<typename io::streaming<T>::chunk>(result.value);
            upstream->seal<typename io::streaming<T>::choke>();
        }

        void
        operator()(const error_type& error) const {
            upstream->send<typename io::streaming<T>::error>(error.code, error.reason);
            upstream->seal<typename io::streaming<T>::choke>();
        }

        void
        operator()(const empty_type&) const {
            upstream->seal<typename io::streaming<T>::choke>();
        }

    private:
        const std::shared_ptr<upstream_t>& upstream;
    };

    // The upstream might be attached during state method invocation, so it has to be synchronized
    // with a mutex - the atomicicity guarantee of the shared_ptr is not enough.
    std::shared_ptr<upstream_t> upstream;
    std::mutex mutex;
};

}} // namespace io::aux

template<class T>
struct deferred {
    template<class R, class Event>
        friend struct io::deferred_slot;

    deferred():
        state(new io::aux::future_state<T>())
    { }

    void
    write(T&& value) {
        state->write(std::forward<T>(value));
    }

    void
    abort(int code, const std::string& reason) {
        state->abort(code, reason);
    }

private:
    const std::shared_ptr<io::aux::future_state<T>> state;
};

template<>
struct deferred<void> {
    template<class R, class Event>
        friend struct io::deferred_slot;

    deferred():
        state(new io::aux::future_state<void>())
    { }

    void
    close() {
        state->close();
    }

    void
    abort(int code, const std::string& reason) {
        state->abort(code, reason);
    }

private:
    const std::shared_ptr<io::aux::future_state<void>> state;
};

} // namespace cocaine

#endif
