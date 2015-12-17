// Copyright 2014 MongoDB Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <mongocxx/collection.hpp>

#include <chrono>
#include <cstdint>
#include <tuple>
#include <utility>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>
#include <bsoncxx/private/helpers.hpp>
#include <bsoncxx/stdx/make_unique.hpp>
#include <bsoncxx/stdx/optional.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write.hpp>
#include <mongocxx/exception/operation.hpp>
#include <mongocxx/exception/query.hpp>
#include <mongocxx/exception/write.hpp>
#include <mongocxx/model/write.hpp>
#include <mongocxx/private/bulk_write.hpp>
#include <mongocxx/private/client.hpp>
#include <mongocxx/private/collection.hpp>
#include <mongocxx/private/database.hpp>
#include <mongocxx/private/libbson.hpp>
#include <mongocxx/private/libmongoc.hpp>
#include <mongocxx/private/pipeline.hpp>
#include <mongocxx/private/read_concern.hpp>
#include <mongocxx/private/read_preference.hpp>
#include <mongocxx/private/write_concern.hpp>
#include <mongocxx/result/bulk_write.hpp>
#include <mongocxx/result/delete.hpp>
#include <mongocxx/result/insert_many.hpp>
#include <mongocxx/result/insert_one.hpp>
#include <mongocxx/result/replace_one.hpp>
#include <mongocxx/result/update.hpp>
#include <mongocxx/write_concern.hpp>

using bsoncxx::document::view_or_value;
using bsoncxx::builder::stream::concatenate;

namespace {

mongocxx::stdx::optional<bsoncxx::document::value> find_and_modify(
    ::mongoc_collection_t* collection, view_or_value filter,
    const ::mongoc_find_and_modify_opts_t* opts) {
    mongocxx::libbson::scoped_bson_t bson_filter{filter};
    mongocxx::libbson::scoped_bson_t reply;
    reply.flag_init();

    ::bson_error_t error;

    bool r = mongocxx::libmongoc::collection_find_and_modify_with_opts(
        collection, bson_filter.bson(), opts, reply.bson(), &error);

    if (!r) {
        auto gle = mongocxx::libmongoc::collection_get_last_error(collection);
        throw mongocxx::exception::write(bsoncxx::helpers::value_from_bson_t(gle),
                                         std::make_tuple(error.message, error.code));
    }

    bsoncxx::document::view result = reply.view();

    if (result["value"].type() == bsoncxx::type::k_null)
        return mongocxx::stdx::optional<bsoncxx::document::value>{};

    bsoncxx::builder::stream::document b{};
    b << concatenate(result["value"].get_document().view());
    return b.extract();
}

// TODO move these to a private header
template <typename T>
class guard {
   public:
    guard(T&& t) : _t{std::move(t)} {
    }

    ~guard() {
        _t();
    }

   private:
    T _t;
};

template <typename T>
guard<T> make_guard(T&& t) {
    return guard<T>{std::forward<T>(t)};
};

}  // namespace

namespace mongocxx {
MONGOCXX_INLINE_NAMESPACE_BEGIN

using namespace libbson;

collection::collection() noexcept = default;
collection::collection(collection&&) noexcept = default;
collection& collection::operator=(collection&&) noexcept = default;
collection::~collection() = default;

collection::operator bool() const noexcept {
    return static_cast<bool>(_impl);
}

stdx::string_view collection::name() const noexcept {
    return stdx::string_view{libmongoc::collection_get_name(_impl->collection_t)};
}

void collection::rename(stdx::string_view new_name, bool drop_target_before_rename) {
    bson_error_t error;

    auto result = libmongoc::collection_rename(_impl->collection_t, _impl->database_name.c_str(),
                                               new_name.data(), drop_target_before_rename, &error);

    if (!result) {
        throw exception::operation(std::make_tuple(error.message, error.code));
    }
}

collection::collection(const database& database, stdx::string_view collection_name)
    : _impl(stdx::make_unique<impl>(
          libmongoc::database_get_collection(database._impl->database_t, collection_name.data()),
          database.name(), database._impl->client_impl)) {
}

collection::collection(const database& database, void* collection)
    : _impl(stdx::make_unique<impl>(static_cast<mongoc_collection_t*>(collection), database.name(),
                                    database._impl->client_impl)) {
}

collection::collection(const collection& c) : _impl{stdx::make_unique<impl>(*(c._impl))} {
}

collection& collection::operator=(const collection& c) {
    _impl = stdx::make_unique<impl>(*(c._impl));
    return *this;
}

stdx::optional<result::bulk_write> collection::bulk_write(const class bulk_write& bulk_write) {
    mongoc_bulk_operation_t* b = bulk_write._impl->operation_t;
    libmongoc::bulk_operation_set_client(b, _impl->client_impl->client_t);
    libmongoc::bulk_operation_set_database(b, _impl->database_name.c_str());
    libmongoc::bulk_operation_set_collection(b, name().data());

    scoped_bson_t reply;
    reply.flag_init();

    bson_error_t error;

    if (!libmongoc::bulk_operation_execute(b, reply.bson(), &error)) {
        throw exception::bulk_write(reply.steal(), std::make_tuple(error.message, error.code));
    }

    result::bulk_write result(reply.steal());

    return stdx::optional<result::bulk_write>(std::move(result));
}

cursor collection::find(view_or_value filter, const options::find& options) {
    using namespace bsoncxx;
    builder::stream::document filter_builder;
    scoped_bson_t filter_bson;
    scoped_bson_t projection{options.projection()};

    filter_builder << "$query" << bsoncxx::types::b_document{filter};

    if (options.modifiers()) {
        filter_builder << builder::stream::concatenate(*options.modifiers());
    }

    if (options.sort()) {
        filter_builder << "$orderby" << bsoncxx::types::b_document{*options.sort()};
    }

    if (options.max_time()) {
        filter_builder << "$maxTimeMS" << options.max_time()->count();
    }

    filter_bson.init_from_static(filter_builder.view());

    const mongoc_read_prefs_t* rp_ptr = NULL;

    if (options.read_preference()) {
        rp_ptr = options.read_preference()->_impl->read_preference_t;
    }

    auto mongoc_cursor = libmongoc::collection_find(
        _impl->collection_t, mongoc_query_flags_t(0), options.skip().value_or(0),
        options.limit().value_or(0), options.batch_size().value_or(0), filter_bson.bson(),
        projection.bson(), rp_ptr);

    if (options.max_await_time()) {
        libmongoc::cursor_set_max_await_time_ms(mongoc_cursor, (*options.max_await_time()).count());
    }

    return cursor{mongoc_cursor};
}

stdx::optional<bsoncxx::document::value> collection::find_one(view_or_value filter,
                                                              const options::find& options) {
    options::find copy(options);
    copy.limit(1);
    cursor cursor = find(filter, copy);
    cursor::iterator it = cursor.begin();
    if (it == cursor.end()) {
        return stdx::nullopt;
    }
    return stdx::optional<bsoncxx::document::value>(bsoncxx::document::value{*it});
}

cursor collection::aggregate(const pipeline& pipeline, const options::aggregate& options) {
    using namespace bsoncxx::builder::stream;

    scoped_bson_t stages(pipeline._impl->view());

    bsoncxx::builder::stream::document b;

    if (options.allow_disk_use()) {
        b << "allowDiskUse" << *options.allow_disk_use();
    }

    if (options.use_cursor()) {
        auto inner = b << "cursor" << open_document;

        if (options.batch_size()) {
            inner << "batchSize" << *options.batch_size();
        }

        inner << close_document;
    }

    if (options.max_time()) {
        b << "maxTimeMS" << options.max_time()->count();
    }

    if (options.bypass_document_validation()) {
        b << "bypassDocumentValidation" << *options.bypass_document_validation();
    }

    scoped_bson_t options_bson(b.view());

    const ::mongoc_read_prefs_t* rp_ptr = NULL;

    if (options.read_preference()) {
        rp_ptr = read_preference()._impl->read_preference_t;
    }

    return cursor(libmongoc::collection_aggregate(_impl->collection_t,
                                                  static_cast<::mongoc_query_flags_t>(0),
                                                  stages.bson(), options_bson.bson(), rp_ptr));
}

void* collection::implementation() const {
    return _impl->collection_t;
}

stdx::optional<result::insert_one> collection::insert_one(view_or_value document,
                                                          const options::insert& options) {
    class bulk_write bulk_op(false);
    bsoncxx::document::element oid{};

    bsoncxx::builder::stream::document new_document;
    if (!document.view()["_id"]) {
        new_document << "_id" << bsoncxx::oid(bsoncxx::oid::init_tag);
        new_document << concatenate(document);
        bulk_op.append(model::insert_one(new_document.view()));
        oid = new_document.view()["_id"];
    } else {
        bulk_op.append(model::insert_one(document));
        oid = document.view()["_id"];
    }

    if (options.write_concern()) {
        bulk_op.write_concern(*options.write_concern());
    }

    if (options.bypass_document_validation()) {
        bulk_op.bypass_document_validation(*options.bypass_document_validation());
    }

    auto result = bulk_write(bulk_op);
    if (!result) {
        return stdx::optional<result::insert_one>();
    }

    return stdx::optional<result::insert_one>(
        result::insert_one(std::move(result.value()), std::move(oid.get_value())));
}

stdx::optional<result::replace_one> collection::replace_one(view_or_value filter,
                                                            view_or_value replacement,
                                                            const options::update& options) {
    class bulk_write bulk_op(false);
    model::replace_one replace_op(filter, replacement);

    if (options.upsert()) replace_op.upsert(options.upsert().value());

    bulk_op.append(replace_op);

    if (options.bypass_document_validation())
        bulk_op.bypass_document_validation(*options.bypass_document_validation());
    if (options.write_concern()) bulk_op.write_concern(*options.write_concern());

    auto result = bulk_write(bulk_op);
    if (!result) {
        return stdx::optional<result::replace_one>();
    }

    return stdx::optional<result::replace_one>(result::replace_one(std::move(result.value())));
};

stdx::optional<result::update> collection::update_many(view_or_value filter, view_or_value update,
                                                       const options::update& options) {
    class bulk_write bulk_op(false);
    model::update_many update_op(filter, update);

    if (options.upsert()) update_op.upsert(options.upsert().value());

    bulk_op.append(update_op);

    if (options.bypass_document_validation())
        bulk_op.bypass_document_validation(*options.bypass_document_validation());
    if (options.write_concern()) bulk_op.write_concern(*options.write_concern());

    auto result = bulk_write(bulk_op);
    if (!result) {
        return stdx::optional<result::update>();
    }

    return stdx::optional<result::update>(result::update(std::move(result.value())));
}

stdx::optional<result::delete_result> collection::delete_many(
    view_or_value filter, const options::delete_options& options) {
    class bulk_write bulk_op(false);
    model::delete_many delete_op(filter);
    bulk_op.append(delete_op);

    if (options.write_concern()) bulk_op.write_concern(*options.write_concern());

    auto result = bulk_write(bulk_op);
    if (!result) {
        return stdx::optional<result::delete_result>();
    }

    return stdx::optional<result::delete_result>(result::delete_result(std::move(result.value())));
}

stdx::optional<result::update> collection::update_one(view_or_value filter, view_or_value update,
                                                      const options::update& options) {
    class bulk_write bulk_op(false);
    model::update_one update_op(filter, update);

    if (options.upsert()) update_op.upsert(options.upsert().value());

    bulk_op.append(update_op);

    if (options.bypass_document_validation())
        bulk_op.bypass_document_validation(*options.bypass_document_validation());
    if (options.write_concern()) bulk_op.write_concern(*options.write_concern());

    auto result = bulk_write(bulk_op);
    if (!result) {
        return stdx::optional<result::update>();
    }

    return stdx::optional<result::update>(result::update(std::move(result.value())));
}

stdx::optional<result::delete_result> collection::delete_one(
    view_or_value filter, const options::delete_options& options) {
    class bulk_write bulk_op(false);
    model::delete_one delete_op(filter);
    bulk_op.append(delete_op);

    if (options.write_concern()) bulk_op.write_concern(*options.write_concern());

    auto result = bulk_write(bulk_op);
    if (!result) {
        return stdx::optional<result::delete_result>();
    }
    return stdx::optional<result::delete_result>(result::delete_result(std::move(result.value())));
}

stdx::optional<bsoncxx::document::value> collection::find_one_and_replace(
    view_or_value filter, view_or_value replacement, const options::find_one_and_replace& options) {
    auto opts = libmongoc::find_and_modify_opts_new();
    auto opts_cleanup = make_guard([&opts] { libmongoc::find_and_modify_opts_destroy(opts); });
    int flags = ::MONGOC_FIND_AND_MODIFY_NONE;
    scoped_bson_t bson_replacement{replacement};
    scoped_bson_t bson_sort{options.sort()};
    scoped_bson_t bson_projection{options.projection()};

    libmongoc::find_and_modify_opts_set_update(opts, bson_replacement.bson());

    if (options.bypass_document_validation()) {
        libmongoc::find_and_modify_opts_set_bypass_document_validation(
            opts, *options.bypass_document_validation());
    }

    if (options.sort()) {
        libmongoc::find_and_modify_opts_set_sort(opts, bson_sort.bson());
    }

    if (options.projection()) {
        libmongoc::find_and_modify_opts_set_fields(opts, bson_projection.bson());
    }

    if (options.upsert().value_or(false)) {
        flags |= ::MONGOC_FIND_AND_MODIFY_UPSERT;
    }

    if (options.return_document() == options::return_document::k_after) {
        flags |= ::MONGOC_FIND_AND_MODIFY_RETURN_NEW;
    }

    // TODO: use options.max_time() when available in the C driver

    libmongoc::find_and_modify_opts_set_flags(opts,
                                              static_cast<::mongoc_find_and_modify_flags_t>(flags));

    return find_and_modify(_impl->collection_t, filter, opts);
}

stdx::optional<bsoncxx::document::value> collection::find_one_and_update(
    view_or_value filter, view_or_value update, const options::find_one_and_update& options) {
    auto opts = libmongoc::find_and_modify_opts_new();
    auto opts_cleanup = make_guard([&opts] { libmongoc::find_and_modify_opts_destroy(opts); });
    int flags = ::MONGOC_FIND_AND_MODIFY_NONE;
    scoped_bson_t bson_update{update};
    scoped_bson_t bson_sort{options.sort()};
    scoped_bson_t bson_projection{options.projection()};

    libmongoc::find_and_modify_opts_set_update(opts, bson_update.bson());

    if (options.bypass_document_validation()) {
        libmongoc::find_and_modify_opts_set_bypass_document_validation(
            opts, *options.bypass_document_validation());
    }

    if (options.sort()) {
        libmongoc::find_and_modify_opts_set_sort(opts, bson_sort.bson());
    }

    if (options.projection()) {
        libmongoc::find_and_modify_opts_set_fields(opts, bson_projection.bson());
    }

    if (options.upsert().value_or(false)) {
        flags |= ::MONGOC_FIND_AND_MODIFY_UPSERT;
    }

    if (options.return_document() == options::return_document::k_after) {
        flags |= ::MONGOC_FIND_AND_MODIFY_RETURN_NEW;
    }

    // TODO: use options.max_time() when available in the C driver

    libmongoc::find_and_modify_opts_set_flags(opts,
                                              static_cast<::mongoc_find_and_modify_flags_t>(flags));

    return find_and_modify(_impl->collection_t, filter, opts);
}

stdx::optional<bsoncxx::document::value> collection::find_one_and_delete(
    view_or_value filter, const options::find_one_and_delete& options) {
    auto opts = libmongoc::find_and_modify_opts_new();
    auto opts_cleanup = make_guard([&opts] { libmongoc::find_and_modify_opts_destroy(opts); });
    auto flags = ::MONGOC_FIND_AND_MODIFY_REMOVE;

    scoped_bson_t bson_sort{options.sort()};
    scoped_bson_t bson_projection{options.projection()};

    if (options.sort()) {
        libmongoc::find_and_modify_opts_set_sort(opts, bson_sort.bson());
    }

    if (options.projection()) {
        libmongoc::find_and_modify_opts_set_fields(opts, bson_projection.bson());
    }

    // TODO: use options.max_time() when available in the C driver

    libmongoc::find_and_modify_opts_set_flags(opts, flags);

    return find_and_modify(_impl->collection_t, filter, opts);
}

std::int64_t collection::count(view_or_value filter, const options::count& options) {
    scoped_bson_t bson_filter{filter};
    bson_error_t error;

    const mongoc_read_prefs_t* rp_ptr = NULL;

    if (options.read_preference()) {
        rp_ptr = options.read_preference()->_impl->read_preference_t;
    }

    // Some options must be added via the options struct
    bsoncxx::builder::stream::document cmd_opts_builder{};

    if (options.max_time()) {
        cmd_opts_builder << "maxTimeMS" << options.max_time()->count();
    }

    if (options.hint()) {
        cmd_opts_builder << concatenate(options.hint()->to_document());
    }

    scoped_bson_t cmd_opts_bson{cmd_opts_builder.view()};

    auto result = libmongoc::collection_count_with_opts(
        _impl->collection_t, static_cast<mongoc_query_flags_t>(0), bson_filter.bson(),
        options.skip().value_or(0), options.limit().value_or(0), cmd_opts_bson.bson(), rp_ptr,
        &error);

    if (result < 0) {
        throw exception::query(std::make_tuple(error.message, error.code));
    }

    return result;
}

bsoncxx::document::value collection::create_index(view_or_value keys,
                                                  const options::index& options) {
    scoped_bson_t bson_keys{keys};
    bson_error_t error;
    ::mongoc_index_opt_geo_t geo_opt{};
    ::mongoc_index_opt_t opt{};
    ::mongoc_index_opt_wt_t wt_opt{};
    libmongoc::index_opt_init(&opt);

    if (options.background()) {
        opt.background = *options.background();
    }

    if (options.unique()) {
        opt.unique = *options.unique();
    }

    if (options.name()) {
        opt.name = options.name()->c_str();
    }

    if (options.sparse()) {
        opt.sparse = *options.sparse();
    }

    if (options.storage_options()) {
        const options::index::wiredtiger_storage_options* wt_options;
        libmongoc::index_opt_wt_init(&wt_opt);

        if (options.storage_options()->type() ==
            ::mongoc_index_storage_opt_type_t::MONGOC_INDEX_STORAGE_OPT_WIREDTIGER) {
            wt_options = static_cast<const options::index::wiredtiger_storage_options*>(
                options.storage_options().get());

            if (wt_options->config_string()) {
                wt_opt.config_str = wt_options->config_string()->c_str();
            }
            opt.storage_options = reinterpret_cast<mongoc_index_opt_storage_t*>(&wt_opt);
        }
    }

    if (options.expire_after_seconds()) {
        opt.expire_after_seconds = *options.expire_after_seconds();
    }

    if (options.version()) {
        opt.v = *options.version();
    }

    if (options.weights()) {
        scoped_bson_t weights{*options.weights()};
        opt.weights = weights.bson();
    }

    if (options.default_language()) {
        opt.default_language = options.default_language()->c_str();
    }

    if (options.language_override()) {
        opt.language_override = options.language_override()->c_str();
    }

    if (options.partial_filter_expression()) {
        scoped_bson_t partial_filter_expression{*options.partial_filter_expression()};
        opt.partial_filter_expression = partial_filter_expression.bson();
    }

    if (options.twod_sphere_version() || options.twod_bits_precision() ||
        options.twod_location_min() || options.twod_location_max() ||
        options.haystack_bucket_size()) {
        libmongoc::index_opt_geo_init(&geo_opt);

        if (options.twod_sphere_version()) {
            geo_opt.twod_sphere_version = *options.twod_sphere_version();
        }

        if (options.twod_bits_precision()) {
            geo_opt.twod_bits_precision = *options.twod_bits_precision();
        }

        if (options.twod_location_min()) {
            geo_opt.twod_location_min = *options.twod_location_min();
        }

        if (options.twod_location_max()) {
            geo_opt.twod_location_max = *options.twod_location_max();
        }

        if (options.haystack_bucket_size()) {
            geo_opt.haystack_bucket_size = *options.haystack_bucket_size();
        }

        opt.geo_options = &geo_opt;
    }

    auto result =
        libmongoc::collection_create_index(_impl->collection_t, bson_keys.bson(), &opt, &error);

    if (!result) {
        throw exception::operation(std::make_tuple(error.message, error.code));
    }

    if (options.name()) {
        return bsoncxx::builder::stream::document{} << "name" << *options.name()
                                                    << bsoncxx::builder::stream::finalize;
    } else {
        return bsoncxx::builder::stream::document{}
               << "name"
               << std::string{libmongoc::collection_keys_to_index_string(bson_keys.bson())}
               << bsoncxx::builder::stream::finalize;
    }
}

cursor collection::distinct(stdx::string_view field_name, view_or_value query,
                            const options::distinct& options) {
    bsoncxx::builder::stream::document command_builder{};
    command_builder << "distinct" << name() << "key" << field_name << "query"
                    << bsoncxx::types::b_document{query};

    if (options.max_time()) {
        command_builder << "maxTimeMS" << options.max_time()->count();
    }

    scoped_bson_t command_bson{command_builder.extract()};

    auto database =
        libmongoc::client_get_database(_impl->client_impl->client_t, _impl->database_name.data());

    auto result = libmongoc::database_command(database, MONGOC_QUERY_NONE, 0, 0, 0,
                                              command_bson.bson(), NULL, NULL);

    return cursor(result);
}

cursor collection::list_indexes() const {
    bson_error_t error;
    auto result = libmongoc::collection_find_indexes(_impl->collection_t, &error);

    if (!result) {
        throw exception::operation(std::make_tuple(error.message, error.code));
    }

    return cursor(result);
}

void collection::drop() {
    bson_error_t error;

    auto result = libmongoc::collection_drop(_impl->collection_t, &error);

    if (!result) {
        throw exception::operation(std::make_tuple(error.message, error.code));
    }
}

void collection::read_concern(class read_concern rc) {
    libmongoc::collection_set_read_concern(_impl->collection_t, rc._impl->read_concern_t);
}

stdx::optional<class read_concern> collection::read_concern() const {
    auto rc = libmongoc::collection_get_read_concern(_impl->collection_t);
    if (!libmongoc::read_concern_get_level(rc)) {
        return stdx::nullopt;
    }
    return {stdx::make_unique<read_concern::impl>(libmongoc::read_concern_copy(rc))};
}

void collection::read_preference(class read_preference rp) {
    libmongoc::collection_set_read_prefs(_impl->collection_t, rp._impl->read_preference_t);
}

class read_preference collection::read_preference() const {
    class read_preference rp(stdx::make_unique<read_preference::impl>(
        libmongoc::read_prefs_copy(libmongoc::collection_get_read_prefs(_impl->collection_t))));
    return rp;
}

void collection::write_concern(class write_concern wc) {
    libmongoc::collection_set_write_concern(_impl->collection_t, wc._impl->write_concern_t);
}

class write_concern collection::write_concern() const {
    class write_concern wc(stdx::make_unique<write_concern::impl>(libmongoc::write_concern_copy(
        libmongoc::collection_get_write_concern(_impl->collection_t))));
    return wc;
}

MONGOCXX_INLINE_NAMESPACE_END
}  // namespace mongocxx