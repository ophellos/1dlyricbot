/*
 * chronos, the cron-job.org execution daemon
 * Copyright (C) 2017 Patrick Schlangen <patrick@schlangen.me>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include "Utils.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

using namespace Chronos;

uint64_t Utils::getTimestampMS()
{
	uint64_t result = 0;
	struct timeval tv;

	if(gettimeofday(&tv, nullptr) == 0)
		result = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;

	return(result);
}

std::string Utils::trim(const std::string &in)
{
	std::string str = in;
	std::string whiteSpaces = " \t\f\v\n\r";
	size_t pos;

	pos = str.find_first_not_of(whiteSpaces);
	if(pos != std::string::npos)
		str.erase(0, pos);
	else
		str.clear();

	pos = str.find_last_not_of(whiteSpaces);
	if(pos != std::string::npos)
		str.erase(pos+1);
	else
		str.clear();

	return(str);
}

void Utils::replace(std::string &str, const std::string &search, const std::string &repl)
{
	size_t pos = 0;
	while((pos = str.find(search, pos)) != std::string::npos)
	{
		str.replace(pos, search.length(), repl);
		pos += search.length();
	}
}

std::string Utils::userPathPart(const int userID)
{
	std::stringstream ss;
	ss << std::hex << userID;
	std::string userIdHex = ss.str();

	std::string result;
	for(size_t i = 0; i < userIdHex.length(); ++i)
	{
		result += userIdHex[i];
		if(i % 2 != 0)
			result += '/';
	}

	if(result[result.size()-1] == '/')
		result.pop_back();

	return result;
}

std::string Utils::toString(int num, int places)
{
	std::string result = std::to_string(num);
	while(result.size() < places)
		result.insert(result.begin(), '0');
	return result;
}

bool Utils::directoryExists(const std::string &path)
{
	struct stat st;

	if(stat(path.c_str(), &st) != 0)
		return false;

	return((st.st_mode & S_IFDIR) == S_IFDIR);
}

bool Utils::mkPath(const std::string &path, const mode_t mode)
{
	std::string currentDir;
	for(const char c : path)
	{
		currentDir += c;
		if(c == '/' && currentDir.size() > 1)
		{
			if(!Utils::directoryExists(currentDir))
			{
				if(mkdir(currentDir.c_str(), mode) != 0)
					return false;
			}
		}
	}
	if(!Utils::directoryExists(currentDir))
	{
		if(mkdir(currentDir.c_str(), mode) != 0)
			return false;
	}
	return true;
}

std::string Utils::toLower(const std::string &str)
{
	std::string result;
	std::transform(str.begin(), str.end(), result.begin(), ::tolower);
	return result;
}

