#pragma once

// A tiny, dependency-free, self-registering test framework — RayTracerBenchTests is a plain
// command-line C++ tool (no XCTest, no Objective-C), and this project already avoids external
// dependencies beyond the vendored Metal bindings, so a single header beats vendoring doctest.

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace TestFramework
{
	struct TestCase
	{
		std::string           name;
		std::function<void()> fn;
	};

	// Meyer's-singleton accessor rather than a plain global, so registration order across
	// translation units (each with its own static TestRegistrar instances) is well-defined.
	inline std::vector<TestCase>& registry()
	{
		static std::vector<TestCase> s_registry;
		return s_registry;
	}

	struct TestRegistrar
	{
		TestRegistrar( const char* name, std::function<void()> fn ) { registry().push_back( { name, std::move( fn ) } ); }
	};

	struct CheckFailure
	{
		std::string message;
	};

	inline int runAll()
	{
		int passed = 0;
		int failed = 0;
		for ( const TestCase& test : registry() )
		{
			try
			{
				test.fn();
				std::printf( "[PASS] %s\n", test.name.c_str() );
				++passed;
			}
			catch ( const CheckFailure& failure )
			{
				std::printf( "[FAIL] %s: %s\n", test.name.c_str(), failure.message.c_str() );
				++failed;
			}
			catch ( const std::exception& ex )
			{
				std::printf( "[FAIL] %s: unexpected exception: %s\n", test.name.c_str(), ex.what() );
				++failed;
			}
		}
		std::printf( "%d passed, %d failed, %d total\n", passed, failed, passed + failed );
		return failed == 0 ? 0 : 1;
	}
}

#define TEST_CASE( name )                                                                        \
	static void          name();                                                                  \
	static ::TestFramework::TestRegistrar registrar_##name( #name, name );                        \
	static void          name()

#define RT_CHECK_MESSAGE( cond, extra )                                                           \
	do                                                                                              \
	{                                                                                               \
		if ( !( cond ) )                                                                             \
		{                                                                                             \
			std::ostringstream oss;                                                                    \
			oss << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond << " " << extra;             \
			throw ::TestFramework::CheckFailure{ oss.str() };                                           \
		}                                                                                             \
	} while ( false )

#define CHECK( cond ) RT_CHECK_MESSAGE( cond, "" )

#define CHECK_NEAR( a, b, eps )                                                                   \
	do                                                                                              \
	{                                                                                               \
		double rt_a = ( a );                                                                         \
		double rt_b = ( b );                                                                         \
		if ( std::fabs( rt_a - rt_b ) > ( eps ) )                                                    \
		{                                                                                             \
			std::ostringstream oss;                                                                    \
			oss << __FILE__ << ":" << __LINE__ << ": CHECK_NEAR failed: " #a " (" << rt_a << ") vs "    \
				<< #b " (" << rt_b << "), eps=" << ( eps );                                              \
			throw ::TestFramework::CheckFailure{ oss.str() };                                           \
		}                                                                                             \
	} while ( false )
