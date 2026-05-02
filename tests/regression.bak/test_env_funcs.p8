pico-8 cartridge // http://www.pico-8.com
version 43
__lua__
-- Tests for _ENV, _G, rawget/rawset, and required global functions.
-- Ported from: test_env, test_functions

#include test_fwk.lua

function test_env()
    test_case("env_is_table", function()
        check_eq(type(_ENV), "table")
    end)
    test_case("g_is_table", function()
        check_eq(type(_G), "table")
    end)
    test_case("env_eq_g", function()
        check_eq(_ENV == _G, true)
    end)
    test_case("rawget_basic", function()
        local t = {a=1, b=2}
        check_eq(rawget(t, "a"), 1)
        check_eq(rawget(t, "b"), 2)
    end)
    test_case("rawget_env", function()
        check_ne(rawget(_ENV, "printh"), nil)
    end)
end

function test_required_functions()
    test_case("select_exists", function() check_ne(select, nil) end)
    test_case("unpack_exists", function() check_ne(unpack, nil) end)
    test_case("rawget_exists", function() check_ne(rawget, nil) end)
    test_case("rawset_exists", function() check_ne(rawset, nil) end)
    test_case("deli_exists", function() check_ne(deli, nil) end)
    test_case("split_exists", function() check_ne(split, nil) end)
    test_case("count_exists", function() check_ne(count, nil) end)
    test_case("next_exists", function() check_ne(next, nil) end)
    test_case("inext_exists", function() check_ne(inext, nil) end)
    test_case("ipairs_exists", function() check_ne(ipairs, nil) end)
end

function _init()
    test_suite("env", test_env)
    test_suite("required_functions", test_required_functions)
    summary()
end
