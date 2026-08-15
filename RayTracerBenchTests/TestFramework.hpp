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
		// Constructing a static TestRegistrar (see the TEST_CASE macro) registers its test function
		// as a side effect, before main() runs.
		TestRegistrar( const char* name, std::function<void()> fn ) { registry().push_back( { name, std::move( fn ) } ); }
	};

	struct CheckFailure
	{
		std::string message;
	};

	// Runs every registered test, printing a [PASS]/[FAIL] line per test and a summary line at the
	// end. Returns a process exit code: 0 if everything passed, 1 if anything failed.
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

// Declares a test function named `name`, registers it via a file-scope static TestRegistrar, then
// opens the function body for the caller to fill in — e.g. `TEST_CASE( myTest ) { CHECK(...); }`.
#define TEST_CASE( name )                                                                        \
	static void          name();                                                                  \
	static ::TestFramework::TestRegistrar registrar_##name( #name, name );                        \
	static void          name()

// Shared implementation behind CHECK(): throws a CheckFailure carrying a file:line-prefixed
// message (plus any extra context) when `cond` is false.
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

// Fails the current test (throwing CheckFailure) unless `cond` is true.
#define CHECK( cond ) RT_CHECK_MESSAGE( cond, "" )

// Fails the current test unless `a` and `b` are within `eps` of each other — for floating-point
// comparisons where exact equality isn't meaningful.
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
