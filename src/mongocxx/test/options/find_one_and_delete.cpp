// Copyright 2016 MongoDB Inc.
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

#include "helpers.hpp"

#include <chrono>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/test_util/catch.hh>
#include <mongocxx/instance.hpp>
#include <mongocxx/options/find_one_and_delete.hpp>

namespace {
using namespace bsoncxx::builder::stream;
using namespace mongocxx;

TEST_CASE("find_one_and_delete", "[find_one_and_delete][option]") {
    instance::current();

    options::find_one_and_delete opts{};

    auto collation = document{} << "locale"
                                << "en_US" << finalize;
    std::chrono::milliseconds ms{400};
    auto projection = document{} << "_id" << false << finalize;
    auto sort = document{} << "x" << -1 << finalize;

    CHECK_OPTIONAL_ARGUMENT(opts, collation, collation.view());
    CHECK_OPTIONAL_ARGUMENT(opts, max_time, ms);
    CHECK_OPTIONAL_ARGUMENT(opts, projection, projection.view());
    CHECK_OPTIONAL_ARGUMENT(opts, sort, sort.view());
}

TEST_CASE("find_one_and_delete equals", "[find_one_and_delete][options]") {
    instance::current();

    options::find_one_and_delete opts1{};
    options::find_one_and_delete opts2{};

    REQUIRE(opts1 == opts2);
}

TEST_CASE("find_one_and_delete inequals", "[find_one_and_delete][options]") {
    instance::current();

    options::find_one_and_delete opts1{};
    opts1.max_time(std::chrono::milliseconds{500});
    options::find_one_and_delete opts2{};

    REQUIRE(opts1 != opts2);
}
}  // namespace
