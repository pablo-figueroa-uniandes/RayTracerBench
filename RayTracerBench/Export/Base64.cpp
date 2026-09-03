#include "Base64.hpp"

namespace
{
	constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	// -1 for any byte that isn't a base64 alphabet character (including '=', handled separately).
	int decodeTable( unsigned char c )
	{
		for ( int i = 0; i < 64; ++i )
			if ( (unsigned char)kTable[ i ] == c )
				return i;
		return -1;
	}
}

std::string base64Encode( const uint8_t* data, size_t len )
{
	std::string out;
	out.reserve( ( ( len + 2 ) / 3 ) * 4 );

	size_t i = 0;
	for ( ; i + 3 <= len; i += 3 )
	{
		uint32_t n = ( (uint32_t)data[ i ] << 16 ) | ( (uint32_t)data[ i + 1 ] << 8 ) | (uint32_t)data[ i + 2 ];
		out.push_back( kTable[ ( n >> 18 ) & 0x3F ] );
		out.push_back( kTable[ ( n >> 12 ) & 0x3F ] );
		out.push_back( kTable[ ( n >> 6 ) & 0x3F ] );
		out.push_back( kTable[ n & 0x3F ] );
	}

	size_t rem = len - i;
	if ( rem == 1 )
	{
		uint32_t n = (uint32_t)data[ i ] << 16;
		out.push_back( kTable[ ( n >> 18 ) & 0x3F ] );
		out.push_back( kTable[ ( n >> 12 ) & 0x3F ] );
		out.push_back( '=' );
		out.push_back( '=' );
	}
	else if ( rem == 2 )
	{
		uint32_t n = ( (uint32_t)data[ i ] << 16 ) | ( (uint32_t)data[ i + 1 ] << 8 );
		out.push_back( kTable[ ( n >> 18 ) & 0x3F ] );
		out.push_back( kTable[ ( n >> 12 ) & 0x3F ] );
		out.push_back( kTable[ ( n >> 6 ) & 0x3F ] );
		out.push_back( '=' );
	}

	return out;
}

std::vector<uint8_t> base64Decode( const std::string& text )
{
	std::vector<uint8_t> out;
	out.reserve( ( text.size() / 4 ) * 3 );

	int      buf[ 4 ];
	int      bufLen = 0;
	for ( char c : text )
	{
		if ( c == '=' || c == '\0' )
			break; // padding (or an accidental embedded NUL) marks the end of real data
		int v = decodeTable( (unsigned char)c );
		if ( v < 0 )
			continue; // skip anything outside the alphabet rather than fail on it
		buf[ bufLen++ ] = v;
		if ( bufLen == 4 )
		{
			out.push_back( (uint8_t)( ( buf[ 0 ] << 2 ) | ( buf[ 1 ] >> 4 ) ) );
			out.push_back( (uint8_t)( ( buf[ 1 ] << 4 ) | ( buf[ 2 ] >> 2 ) ) );
			out.push_back( (uint8_t)( ( buf[ 2 ] << 6 ) | buf[ 3 ] ) );
			bufLen = 0;
		}
	}

	// Leftover partial group (1 real base64 digit is impossible/invalid and is dropped; 2 or 3
	// digits recover 1 or 2 trailing bytes respectively — the inverse of base64Encode's rem==1/2
	// padding cases above).
	if ( bufLen == 2 )
		out.push_back( (uint8_t)( ( buf[ 0 ] << 2 ) | ( buf[ 1 ] >> 4 ) ) );
	else if ( bufLen == 3 )
	{
		out.push_back( (uint8_t)( ( buf[ 0 ] << 2 ) | ( buf[ 1 ] >> 4 ) ) );
		out.push_back( (uint8_t)( ( buf[ 1 ] << 4 ) | ( buf[ 2 ] >> 2 ) ) );
	}

	return out;
}
