#define CATCH_CONFIG_MAIN

#include "../includes/transforms/ExcludeFiles.h"
#include "catch.hpp"

#include "test_helper.h"

TEST_CASE("Files can be excluded", "[ExcludeFiles]") {
  VecEvents vec(new std::vector<VecEvents::element_type::value_type>());

  SECTION("exclude file name") {
    std::string regex = "~[a-zA-Z_][a-zA-Z_0-9]*\\.tmp";
    ExcludeFiles exFiles(regex);

    SECTION("file name does not match") {
      vec->emplace_back(new Event(EventType::CREATED, "", "foobar.txt", ""));
      vec->emplace_back(new Event(EventType::CREATED, "", "~bla.tmmp", ""));

      auto transformed = exFiles(std::move(vec));
      REQUIRE(transformed->size() == 2);
    }

    SECTION("one file name does and another not match") {
      vec->emplace_back(new Event(EventType::CREATED, "", "video.mp4", ""));
      vec->emplace_back(new Event(EventType::CREATED, "", "~videomp4.tmp", ""));

      auto transformed = exFiles(std::move(vec));
      REQUIRE(transformed->size() == 1);
    }

    SECTION("file name matches the regular expression") {
      vec->emplace_back(new Event(EventType::CREATED, "", "~tmpfile.tmp", ""));

      auto transformed = exFiles(std::move(vec));
      REQUIRE(transformed->size() == 0);
    }
  }
}
