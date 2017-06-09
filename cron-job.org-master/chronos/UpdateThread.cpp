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

#include "UpdateThread.h"

#include <iostream>

#include <stdlib.h>
#include <time.h>

#include "App.h"
#include "Notification.h"
#include "SQLite.h"
#include "Utils.h"

using namespace Chronos;

UpdateThread *UpdateThread::instance = nullptr;

UpdateThread::UpdateThread()
{
	if(UpdateThread::instance != nullptr)
		throw std::runtime_error("Update thread instance already exists");

	UpdateThread::instance 	= this;

	maxFailures 			= App::getInstance()->config->getInt("max_failures");
	userDbFilePathScheme 	= App::getInstance()->config->get("user_db_file_path_scheme");
	userDbFileNameScheme 	= App::getInstance()->config->get("user_db_file_name_scheme");
}

UpdateThread::~UpdateThread()
{
	UpdateThread::instance = nullptr;
}

UpdateThread *UpdateThread::getInstance()
{
	if(UpdateThread::instance == nullptr)
		throw std::runtime_error("No update thread instance available");
	return(UpdateThread::instance);
}

void UpdateThread::addResult(std::unique_ptr<JobResult> result)
{
	std::lock_guard<std::mutex> lg(queueMutex);
	queue.push(std::move(result));
	queueSignal.notify_one();
}

void UpdateThread::storeResult(const std::unique_ptr<JobResult> &result)
{
	std::string userPathPart = Utils::userPathPart(result->userID);

	struct tm tmStruct = { 0 };
	time_t tmTime = result->datePlanned / 1000;
	if(gmtime_r(&tmTime, &tmStruct) == nullptr)
		throw std::runtime_error("gmtime_r returned nullptr");

	// e.g. /var/lib/cron-job.org/%u
	std::string dbDirPath = userDbFilePathScheme;
	Utils::replace(dbDirPath, "%u", userPathPart);
	if(!Utils::directoryExists(dbDirPath))
		Utils::mkPath(dbDirPath);

	// e.g. joblog-%m-%d.db
	std::string dbFileName = userDbFileNameScheme;
	Utils::replace(dbFileName, "%d", Utils::toString(tmStruct.tm_mday, 2));
	Utils::replace(dbFileName, "%m", Utils::toString(tmStruct.tm_mon, 2));

	std::string dbFilePath = dbDirPath + "/" + dbFileName;
	int jobLogID = 0;
	int jobLogIDDay = tmStruct.tm_mday;
	int jobLodIDMonth = tmStruct.tm_mon;

	try
	{
		std::unique_ptr<SQLite_DB> userDB = std::make_unique<SQLite_DB>(dbFilePath.c_str());

		userDB->prepare("PRAGMA synchronous = OFF")->execute();

		userDB->prepare("CREATE TABLE IF NOT EXISTS \"joblog\"("
			"	\"joblogid\" INTEGER PRIMARY KEY ASC,"
			"	\"jobid\" INTEGER NOT NULL,"
			"	\"date\" INTEGER NOT NULL,"
			"	\"date_planned\" INTEGER NOT NULL,"
			"	\"jitter\" INTEGER NOT NULL,"
			"	\"url\" TEXT NOT NULL,"
			"	\"duration\" INTEGER NOT NULL,"
			"	\"status\" INTEGER NOT NULL,"
			"	\"status_text\" TEXT NOT NULL,"
			"	\"http_status\" INTEGER NOT NULL,"
			"	\"created\" INTEGER NOT NULL"
			")")->execute();
		userDB->prepare("CREATE INDEX IF NOT EXISTS \"idx_joblog_jobid\" ON \"joblog\" (\"jobid\")")->execute();

		userDB->prepare("CREATE TABLE IF NOT EXISTS \"joblog_response\"("
			"	\"joblogid\" INTEGER PRIMARY KEY,"
			"	\"jobid\" INTEGER NOT NULL,"
			"	\"date\" INTEGER NOT NULL,"
			"	\"headers\" TEXT NOT NULL,"
			"	\"body\" TEXT NOT NULL,"
			"	\"created\" INTEGER NOT NULL"
			")")->execute();

		auto stmt = userDB->prepare("INSERT INTO \"joblog\"(\"jobid\",\"date\",\"date_planned\",\"jitter\",\"url\",\"duration\",\"status\",\"status_text\",\"http_status\",\"created\") "
			"VALUES(:jobid,:date,:date_planned,:jitter,:url,:duration,:status,:status_text,:http_status,strftime('%s', 'now'))");
		stmt->bind(":jobid", 		result->jobID);
		stmt->bind(":date", 		static_cast<int>(result->dateStarted / 1000));
		stmt->bind(":date_planned", static_cast<int>(result->datePlanned / 1000));
		stmt->bind(":jitter", 		result->jitter);
		stmt->bind(":url", 			result->url);
		stmt->bind(":duration", 	result->duration);
		stmt->bind(":status", 		static_cast<int>(result->status));
		stmt->bind(":status_text", 	result->statusText);
		stmt->bind(":http_status", 	result->httpStatus);
		stmt->execute();

		jobLogID = static_cast<int>(userDB->insertId());

		if(result->saveResponses && (!result->responseHeaders.empty() || !result->responseBody.empty()))
		{
			stmt = userDB->prepare("INSERT INTO \"joblog_response\"(\"joblogid\",\"jobid\",\"date\",\"headers\",\"body\",\"created\") "
				"VALUES(:joblogid,:jobid,:date,:headers,:body,strftime('%s', 'now'))");
			stmt->bind(":joblogid", jobLogID);
			stmt->bind(":jobid", 	result->jobID);
			stmt->bind(":date", 	static_cast<int>(result->dateStarted / 1000));
			stmt->bind(":headers", 	result->responseHeaders);
			stmt->bind(":body", 	result->responseBody);
			stmt->execute();
		}
	}
	catch(const std::exception &ex)
	{
		std::cout << "Error SQLite query: " << ex.what() << std::endl;
		return;
	}

	if(result->status == JOBSTATUS_OK || result->status == JOBSTATUS_FAILED_TIMEOUT)
	{
		db->query("UPDATE `job` SET `last_status`=%d,`last_fetch`=%d,`last_duration`=%d,`fail_counter`=0 WHERE `jobid`=%d",
			static_cast<int>(result->status),
			static_cast<int>(result->dateStarted / 1000),
			static_cast<int>(result->duration),
			result->jobID);
	}
	else
	{
		db->query("UPDATE `job` SET `last_status`=%d,`last_fetch`=%d,`last_duration`=%d,`fail_counter`=`fail_counter`+1 WHERE `jobid`=%d",
			static_cast<int>(result->status),
			static_cast<int>(result->dateStarted / 1000),
			static_cast<int>(result->duration),
			result->jobID);
	}

	// get (new) fail counter
	int failCounter = 0;
	MYSQL_ROW row;
	auto res = db->query("SELECT `fail_counter` FROM `job` WHERE `jobid`=%d",
		result->jobID);
	while((row = res->fetchRow()) != NULL)
	{
		failCounter = atoi(row[0]);
	}
	res.reset();

	bool createNotificationRow = false;
	NotificationType_t notificationType;

	// disable job?
	if(failCounter > maxFailures)
	{
		// disable
		db->query("UPDATE `job` SET `enabled`=0,`fail_counter`=0 WHERE `jobid`=%d",
			result->jobID);

		// notify?
		if(result->notifyDisable)
		{
			createNotificationRow 		= true;
			notificationType 			= NOTIFICATION_TYPE_DISABLE;
		}
	}

	// send failure notification?
	if(result->notifyFailure
		&& result->status != JOBSTATUS_OK
		&& failCounter == 1)
	{
		createNotificationRow 		= true;
		notificationType 			= NOTIFICATION_TYPE_FAILURE;
	}

	// send success notification?
	if(result->notifySuccess
		&& result->status == JOBSTATUS_OK
		&& result->oldFailCounter > 0
		&& failCounter == 0)
	{
		createNotificationRow 		= true;
		notificationType 			= NOTIFICATION_TYPE_SUCCESS;
	}

	if(createNotificationRow)
	{
		db->query("INSERT INTO `notification`(`jobid`,`joblogid`,`date`,`type`) "
			"VALUES(%d,%d,%d,%d)",
			result->jobID,
			jobLogID,
			static_cast<int>(time(NULL)),
			static_cast<int>(notificationType));
	}
}

void UpdateThread::stopThread()
{
	stop = true;

	std::lock_guard<std::mutex> lg(queueMutex);
	queueSignal.notify_one();
}

void UpdateThread::run()
{
	std::cout << "UpdateThread::run(): Entered" << std::endl;

	decltype(queue) tempQueue;
	db = App::getInstance()->createMySQLConnection();

	stop = false;
	while(!stop)
	{
		{
			std::unique_lock<std::mutex> lock(queueMutex);
			if(queue.empty())
				queueSignal.wait(lock);
			queue.swap(tempQueue);
		}

		auto numJobs = tempQueue.size();
		if(numJobs > 100)
			std::cout << "UpdateThread::run(): " << numJobs << " update jobs fetched" << std::endl;

		time_t tStart = time(nullptr);
		if(!tempQueue.empty())
		{
			while(!tempQueue.empty())
			{
				std::unique_ptr<JobResult> res = std::move(tempQueue.front());
				tempQueue.pop();
				storeResult(res);
			}
		}
		time_t tEnd = time(nullptr);

		if(numJobs > 100)
			std::cout << "UpdateThread::run(): Processing " << numJobs << " took " << (tEnd-tStart) << " seconds" << std::endl;
	}

	std::cout << "UpdateThread::run(): Finished" << std::endl;
}
