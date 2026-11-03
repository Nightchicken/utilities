#ifndef STRINGS_H
#define STRINGS_H
#include <string.h>

/**
 * @file strings.h
 * @brief Null Byteless String format, uses a length variable along side of a const char* string
 *
 *
 * @note StringView are more of slices than tradition c strings,
 * they need the format and args to be printed in a printf,
 * can use the fact c allows two strings next to eachother as one string
 *
 *
 * @code
 *     StringView s = sv("string");
 *     printf(SV_FMT"\n",SV_ARGS(s));
 * @endcode
 */
typedef struct{
	const char* data;
	size_t length;
}StringView;


#define SV_FMT "%.*s"
#define SV_ARGS(s) (int)(s).length, (s).data


/**
 * @brief init function for the StringView
 *
 * @param String Literal.
 * @return StringView
 */
StringView sv(const char* String){
	StringView s = 
	{
		.data = String,
		.length = strlen(String)
	};
	return s;
}

/**
 * @brief String View slice, used to remove letters from the right side
 *
 * @param a String View Pointer
 * @param b Number of characters to remove from the right side
 * @return No direct return, the original String View has N less characters from the end
 */
void sv_chop_right(StringView *String, size_t n){
	if ( n > String->length ) return;
	String->length -= n;
}
/**
 * @brief String View slice, used to remove letters from the left side
 *
 * @param a String View Pointer
 * @param b Number of characters to remove from the left side
 * @return No direct return, the original String View has N less characters from the start
 */
void sv_chop_left(StringView *String, size_t n){
	if ( n > String->length ) return;
	String->length -= n;
	String->data += n;
}
/**
 * @brief String View slice, removes charecters from the left side that meet the function input criteria
 *
 * @param a String View Pointer
 * @param b Function pointer to compare characters to
 * @return No direct return, the original String View has all the begining characters that meat the criteria moved
 */
void sv_trim_left(StringView *String, int (*isType)(int)){
	while(isType(*String->data)){
		String->data += 1;
		String->length -= 1;
	}
}
/**
 * @brief String View slice, removes charecters from the right side that meet the function input criteria
 *
 * @param a String View Pointer
 * @param b Function pointer to compare characters to
 * @return No direct return, the original String View has all the end characters that meat the criteria moved
 */
void sv_trim_right(StringView *String, int (*isType)(int)){
	while(isType(String->data[String->length-1])){
		String->length -= 1;
	}
}
/**
 * @brief String View slice, removes charecters from either side that meet the function input criteria
 *
 * @param a String View Pointer
 * @param b Function pointer to compare characters to
 * @return No direct return, the original String View has all the end and beginning characters that meat the criteria moved
 */
void sv_trim(StringView *String, int (*isType)(int)){
	sv_trim_left(String,isType);
	sv_trim_right(String,isType);
}
/**
 * @brief String View splitting by a delimmiter, creates two portions
 *
 * @param a String View Pointer
 * @param b which character to cut
 * @return String View with a first portion, the pointer gets the second portion.
 *         If the delimiter is absent the whole view is returned and the
 *         original is left empty.
 */
StringView sv_split(StringView* String, char c){
	StringView s = { .data = String->data, .length = String->length };
	size_t position = 0;
	while (position < String->length){
		if (String->data[position] == c){
			s.length = position;
			sv_chop_left(String, position + 1);
			return s;
		}
		position += 1;
	}
	sv_chop_left(String, String->length);
	return s;
}


#endif
