/*
 * Copyright (C) 2018-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "atomic_cell.hh"
#include "atomic_cell_or_collection.hh"
#include "counters.hh"
#include "types/types.hh"

atomic_cell atomic_cell::make_dead(api::timestamp_type timestamp, gc_clock::time_point deletion_time) {
    return atomic_cell_type::make_dead(timestamp, deletion_time);
}

atomic_cell atomic_cell::make_live(const abstract_type& type, api::timestamp_type timestamp, bytes_view value, atomic_cell::collection_member cm) {
    return atomic_cell_type::make_live(timestamp, single_fragment_range(value));
}

atomic_cell atomic_cell::make_live(const abstract_type& type, api::timestamp_type timestamp, managed_bytes_view value, atomic_cell::collection_member cm) {
    return atomic_cell_type::make_live(timestamp, fragment_range(value));
}

atomic_cell atomic_cell::make_live(const abstract_type& type, api::timestamp_type timestamp, ser::buffer_view<bytes_ostream::fragment_iterator> value, atomic_cell::collection_member cm) {
    return atomic_cell_type::make_live(timestamp, value);
}

atomic_cell atomic_cell::make_live(const abstract_type& type, api::timestamp_type timestamp, const fragmented_temporary_buffer::view& value, collection_member cm)
{
    return atomic_cell_type::make_live(timestamp, value);
}

atomic_cell atomic_cell::make_live(const abstract_type& type, api::timestamp_type timestamp, bytes_view value,
                             gc_clock::time_point expiry, gc_clock::duration ttl, atomic_cell::collection_member cm) {
    return atomic_cell_type::make_live(timestamp, single_fragment_range(value), expiry, ttl);
}

atomic_cell atomic_cell::make_live(const abstract_type& type, api::timestamp_type timestamp, managed_bytes_view value,
                             gc_clock::time_point expiry, gc_clock::duration ttl, atomic_cell::collection_member cm) {
    return atomic_cell_type::make_live(timestamp, fragment_range(value), expiry, ttl);
}

atomic_cell atomic_cell::make_live(const abstract_type& type, api::timestamp_type timestamp, ser::buffer_view<bytes_ostream::fragment_iterator> value,
                             gc_clock::time_point expiry, gc_clock::duration ttl, atomic_cell::collection_member cm) {
    return atomic_cell_type::make_live(timestamp, value, expiry, ttl);
}

atomic_cell atomic_cell::make_live(const abstract_type& type, api::timestamp_type timestamp, const fragmented_temporary_buffer::view& value,
                                   gc_clock::time_point expiry, gc_clock::duration ttl, collection_member cm)
{
    return atomic_cell_type::make_live(timestamp, value, expiry, ttl);
}

atomic_cell atomic_cell::make_live_counter_update(api::timestamp_type timestamp, int64_t value) {
    return atomic_cell_type::make_live_counter_update(timestamp, value);
}

atomic_cell atomic_cell::make_live_uninitialized(const abstract_type& type, api::timestamp_type timestamp, size_t size) {
    return atomic_cell_type::make_live_uninitialized(timestamp, size);
}

atomic_cell::atomic_cell(const abstract_type& type, atomic_cell_view other)
    : _data(other._view) {
    set_view(_data);
}

// Based on:
//  - org.apache.cassandra.db.AbstractCell#reconcile()
//  - org.apache.cassandra.db.BufferExpiringCell#reconcile()
//  - org.apache.cassandra.db.BufferDeletedCell#reconcile()
std::strong_ordering
compare_atomic_cell_for_merge(atomic_cell_view left, atomic_cell_view right) {
    if (left.timestamp() != right.timestamp()) {
        return left.timestamp() <=> right.timestamp();
    }
    if (left.is_live() != right.is_live()) {
        return left.is_live() ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    if (left.is_live()) {
        auto c = compare_unsigned(left.value(), right.value()) <=> 0;
        if (c != 0) {
            return c;
        }
        if (left.is_live_and_has_ttl() != right.is_live_and_has_ttl()) {
            // prefer expiring cells.
            return left.is_live_and_has_ttl() ? std::strong_ordering::greater : std::strong_ordering::less;
        }
        if (left.is_live_and_has_ttl()) {
            if (left.expiry() != right.expiry()) {
                return left.expiry() <=> right.expiry();
            } else {
                // prefer the cell that was written later,
                // so it survives longer after it expires, until purged.
                return right.ttl() <=> left.ttl();
            }
        }
    } else {
        // Both are deleted

        // Origin compares big-endian serialized deletion time. That's because it
        // delegates to AbstractCell.reconcile() which compares values after
        // comparing timestamps, which in case of deleted cells will hold
        // serialized expiry.
        return (uint64_t) left.deletion_time().time_since_epoch().count()
                <=> (uint64_t) right.deletion_time().time_since_epoch().count();
    }
    return std::strong_ordering::equal;
}

atomic_cell_or_collection atomic_cell_or_collection::copy(const abstract_type& type) const {
    if (_data.empty()) {
        return atomic_cell_or_collection();
    }
    return atomic_cell_or_collection(managed_bytes(_data));
}

atomic_cell_or_collection::atomic_cell_or_collection(const abstract_type& type, atomic_cell_view acv)
    : _data(acv._view)
{
}

bool atomic_cell_or_collection::equals(const abstract_type& type, const atomic_cell_or_collection& other) const
{
    if (_data.empty() || other._data.empty()) {
        return _data.empty() && other._data.empty();
    }

    if (type.is_atomic()) {
        auto a = atomic_cell_view::from_bytes(type, _data);
        auto b = atomic_cell_view::from_bytes(type, other._data);
        if (a.timestamp() != b.timestamp()) {
            return false;
        }
        if (a.is_live() != b.is_live()) {
            return false;
        }
        if (a.is_live()) {
            if (a.is_counter_update() != b.is_counter_update()) {
                return false;
            }
            if (a.is_counter_update()) {
                return a.counter_update_value() == b.counter_update_value();
            }
            if (a.is_live_and_has_ttl() != b.is_live_and_has_ttl()) {
                return false;
            }
            if (a.is_live_and_has_ttl()) {
                if (a.ttl() != b.ttl() || a.expiry() != b.expiry()) {
                    return false;
                }
            }
            return a.value() == b.value();
        }
        return a.deletion_time() == b.deletion_time();
    } else {
        return as_collection_mutation().data == other.as_collection_mutation().data;
    }
}

size_t atomic_cell_or_collection::external_memory_usage(const abstract_type& t) const
{
    return _data.external_memory_usage();
}

std::ostream&
operator<<(std::ostream& os, const atomic_cell_view& acv) {
    if (acv.is_live()) {
        fmt::print(os, "atomic_cell{{{},ts={:d},expiry={:d},ttl={:d}}}",
            acv.is_counter_update()
                    ? "counter_update_value=" + to_sstring(acv.counter_update_value())
                    : to_hex(to_bytes(acv.value())),
            acv.timestamp(),
            acv.is_live_and_has_ttl() ? acv.expiry().time_since_epoch().count() : -1,
            acv.is_live_and_has_ttl() ? acv.ttl().count() : 0);
    } else {
        fmt::print(os, "atomic_cell{{DEAD,ts={:d},deletion_time={:d}}}",
            acv.timestamp(), acv.deletion_time().time_since_epoch().count());
    }
    return os;
}

std::ostream&
operator<<(std::ostream& os, const atomic_cell& ac) {
    return os << atomic_cell_view(ac);
}

std::ostream&
operator<<(std::ostream& os, const atomic_cell_view::printer& acvp) {
    auto& type = acvp._type;
    auto& acv = acvp._cell;
    if (acv.is_live()) {
        std::ostringstream cell_value_string_builder;
        if (type.is_counter()) {
            if (acv.is_counter_update()) {
                cell_value_string_builder << "counter_update_value=" << acv.counter_update_value();
            } else {
                cell_value_string_builder << "shards: ";
                auto ccv = counter_cell_view(acv);
                cell_value_string_builder << ::join(", ", ccv.shards());
            }
        } else {
            cell_value_string_builder << type.to_string(to_bytes(acv.value()));
        }
        fmt::print(os, "atomic_cell{{{},ts={:d},expiry={:d},ttl={:d}}}",
            cell_value_string_builder.str(),
            acv.timestamp(),
            acv.is_live_and_has_ttl() ? acv.expiry().time_since_epoch().count() : -1,
            acv.is_live_and_has_ttl() ? acv.ttl().count() : 0);
    } else {
        fmt::print(os, "atomic_cell{{DEAD,ts={:d},deletion_time={:d}}}",
            acv.timestamp(), acv.deletion_time().time_since_epoch().count());
    }
    return os;
}

std::ostream&
operator<<(std::ostream& os, const atomic_cell::printer& acp) {
    return operator<<(os, static_cast<const atomic_cell_view::printer&>(acp));
}

std::ostream& operator<<(std::ostream& os, const atomic_cell_or_collection::printer& p) {
    if (p._cell._data.empty()) {
        return os << "{ null atomic_cell_or_collection }";
    }
    os << "{ ";
    if (p._cdef.type->is_multi_cell()) {
        os << "collection ";
        auto cmv = p._cell.as_collection_mutation();
        os << collection_mutation_view::printer(*p._cdef.type, cmv);
    } else {
        os << atomic_cell_view::printer(*p._cdef.type, p._cell.as_atomic_cell(p._cdef));
    }
    return os << " }";
}
