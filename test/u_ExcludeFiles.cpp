#define CATCH_CONFIG_MAIN

#include "catch.hpp"
#include "../includes/transforms/ExcludeFiles.h"

TEST_CASE( "Files can be excluded", "[ExcludeFiles]" )
{
  VecEvents vec(new std::vector<VecEvents::element_type::value_type>());

  SECTION( "exclude file name" )
  {
    std::string regex = "~[a-zA-Z_][a-zA-Z_0-9]*\\.tmp";
    ExcludeFiles exFiles(regex);

    SECTION( "file name does match")
    {
        vec->emplace_back(new Event(EventType::CREATED, "", "foobar.txt", ""));
        vec->emplace_back(new Event(EventType::CREATED, "", "~bla.tmmp", ""));

        auto transformed = exFiles(std::move(vec));
        REQUIRE( transformed->size() == 2 );
    }

    SECTION( "file name matches the regular expression")
    {
        vec->emplace_back(new Event(EventType::CREATED, "", "~tmpfile.tmp", ""));

        auto transformed = exFiles(std::move(vec));
        REQUIRE( transformed->size() == 0 );
    }
  }
}
