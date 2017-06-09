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

#include "App.h"

#include <stdexcept>
#include <iostream>
#include <functional>

#include <civil_time.h>
#include <time_zone.h>

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#include "UpdateThread.h"
#include "WorkerThread.h"
#include "Config.h"

using namespace Chronos;

App *App::instance = nullptr;

App::App(int argc, char *argv[])
{
	if(App::instance != nullptr)
		throw std::runtime_error("App instance already exists");

	if(argc != 2)
		throw std::runtime_error(std::string("Usage: ") + std::string(argv[0]) + std::string(" [config-file]"));

	this->config 		= std::make_shared<Config>(argv[1]);
	App::instance		= this;
}

App::~App()
{
	App::instance		= nullptr;
}

App *App::getInstance()
{
        if(App::instance == nullptr)
                throw std::runtime_error("No app instance available");
        return(App::instance);
}

void App::processJobs(time_t forTime, time_t plannedTime)
{
	std::cout 	<< "App::processJobs(): Called for "
				<< "forTime = " << forTime << ", "
				<< "plannedTime = " << plannedTime
				<< std::endl;

	struct tm *t = gmtime(&plannedTime);
	std::shared_ptr<WorkerThread> wt = std::make_shared<WorkerThread>(t->tm_mday, t->tm_mon+1, t->tm_year+1900, t->tm_hour, t->tm_min);

	MYSQL_ROW row;
	auto res = db->query("SELECT DISTINCT(`timezone`) FROM `user`");
	while((row = res->fetchRow()) != nullptr)
	{
		std::string timeZone(row[0]);

		cctz::time_zone tz;
		if(!cctz::load_time_zone(timeZone, &tz))
		{
			std::cout << "App::processJobs(): Failed to load time zone: " << timeZone << ", skipping" << std::endl;
			continue;
		}

		auto civilTime = cctz::convert(std::chrono::system_clock::from_time_t(forTime), tz);
		auto cWDay = cctz::get_weekday(cctz::civil_day(civilTime));
		int wday = -1;
		switch(cWDay)
		{
		case cctz::weekday::monday:	wday = 1;	break;
		case cctz::weekday::tuesday:	wday = 2;	break;
		case cctz::weekday::wednesday:	wday = 3;	break;
		case cctz::weekday::thursday:	wday = 4;	break;
		case cctz::weekday::friday:	wday = 5;	break;
		case cctz::weekday::saturday:	wday = 6;	break;
		case cctz::weekday::sunday:	wday = 0;	break;
		default:			wday = -1;	break;
		}

		processJobsForTimeZone(civilTime.hour(), civilTime.minute(), civilTime.month(), civilTime.day(), wday, civilTime.year(),
			plannedTime, timeZone, wt);
	}

	if(!wt->empty())
	{
		std::cout << "App::processJobs(): Starting worker thread" << std::endl;
		wt->run();
	}
	else
	{
		std::cout << "App::processJobs(): No jobs" << std::endl;
	}
}

void App::processJobsForTimeZone(int hour, int minute, int month, int mday, int wday, int year, time_t timestamp, const std::string &timeZone,
	const std::shared_ptr<WorkerThread> &wt)
{
	std::cout 	<< "App::processJobsForTimeZone(): Called for "
				<< "hour = " << hour << ", "
				<< "minute = " << minute << ", "
				<< "month = " << month << ", "
				<< "mday = " << mday << ", "
				<< "wday = " << wday << ", "
				<< "timestamp = " << timestamp << ", "
				<< "timeZone = " << timeZone
				<< std::endl;

	auto res = db->query("SELECT TRIM(`url`),`job`.`jobid`,`auth_enable`,`auth_user`,`auth_pass`,`notify_failure`,`notify_success`,`notify_disable`,`fail_counter`,`save_responses`,`job`.`userid`,`request_method`,COUNT(`job_header`.`jobheaderid`),`job_body`.`body` FROM `job` "
									"INNER JOIN `job_hours` ON `job_hours`.`jobid`=`job`.`jobid` "
									"INNER JOIN `job_mdays` ON `job_mdays`.`jobid`=`job`.`jobid` "
									"INNER JOIN `job_wdays` ON `job_wdays`.`jobid`=`job`.`jobid` "
									"INNER JOIN `job_minutes` ON `job_minutes`.`jobid`=`job`.`jobid` "
									"INNER JOIN `job_months` ON `job_months`.`jobid`=`job`.`jobid` "
									"INNER JOIN `user` ON `job`.`userid`=`user`.`userid` "
									"LEFT JOIN `job_header` ON `job_header`.`jobid`=`job`.`jobid` "
									"LEFT JOIN `job_body` ON `job_body`.`jobid`=`job`.`jobid` "
									"WHERE (`hour`=-1 OR `hour`=%d) "
									"AND (`minute`=-1 OR `minute`=%d) "
									"AND (`mday`=-1 OR `mday`=%d) "
									"AND (`wday`=-1 OR `wday`=%d) "
									"AND (`month`=-1 OR `month`=%d) "
									"AND `user`.`timezone`='%q' "
									"AND `enabled`=1 "
									"GROUP BY `job`.`jobid` "
									"ORDER BY `fail_counter` ASC, `last_duration` ASC",
									hour, minute, mday, wday, month, timeZone.c_str());

	int jobCount = res->numRows();
	std::cout << "App::processJobs(): " << jobCount << " jobs found" << std::endl;

	if(jobCount > 0)
	{
		MYSQL_ROW row;
		while((row = res->fetchRow()) != nullptr)
		{
			HTTPRequest *req = HTTPRequest::fromURL(row[0], atoi(row[10]));
			req->result->jobID 			= atoi(row[1]);
			req->result->datePlanned	= (uint64_t)timestamp * 1000;
			req->result->notifyFailure 	= strcmp(row[5], "1") == 0;
			req->result->notifySuccess 	= strcmp(row[6], "1") == 0;
			req->result->notifyDisable 	= strcmp(row[7], "1") == 0;
			req->result->oldFailCounter	= atoi(row[8]);
			req->result->saveResponses	= strcmp(row[9], "1") == 0;
			if(atoi(row[2]) == 1)
			{
				req->useAuth 		= true;
				req->authUsername 	= row[3];
				req->authPassword 	= row[4];
			}
			req->requestMethod		= static_cast<RequestMethod>(atoi(row[11]));

			if(row[12] != NULL && atoi(row[12]) > 0)
			{
				auto headerRes = db->query("SELECT `key`,`value` FROM `job_header` WHERE `jobid`=%s",
					row[1]);
				while(MYSQL_ROW row = headerRes->fetchRow())
				{
					req->requestHeaders.push_back({ std::string(row[0]), std::string(row[1]) });
				}
			}

			if(row[13] != NULL)
			{
				req->requestBody	= row[13];
			}

			wt->addJob(req);
		}
	}

	res.reset();

	std::cout << "App::processJobsForTimeZone(): Finished" << std::endl;
}

void App::signalHandler(int sig)
{
	if(sig == SIGINT)
		App::getInstance()->stop = true;
}

int App::run()
{
	curl_global_init(CURL_GLOBAL_ALL);
	MySQL_DB::libInit();

	db = createMySQLConnection();
	startUpdateThread();

	signal(SIGINT, App::signalHandler);

	bool firstLoop = true;
	struct tm lastTime = { 0 };
	int jitterCorrectionOffset = calcJitterCorrectionOffset();
	while(!stop)
	{
		time_t currentTime = time(nullptr) + jitterCorrectionOffset;
		struct tm *t = localtime(&currentTime);

		if(t->tm_min > lastTime.tm_min
			|| t->tm_hour > lastTime.tm_hour
			|| t->tm_mday > lastTime.tm_mday
			|| t->tm_mon > lastTime.tm_mon
			|| t->tm_year > lastTime.tm_year)
		{
			// update last time
			memcpy(&lastTime, t, sizeof(struct tm));

			if(!firstLoop || t->tm_sec == 59 - jitterCorrectionOffset)
			{
				processJobs(currentTime, currentTime - t->tm_sec);
				jitterCorrectionOffset = calcJitterCorrectionOffset();
			}

			firstLoop = false;
		}
		else
		{
			usleep(100*1000);
		}
	}

	this->stopUpdateThread();

	MySQL_DB::libCleanup();
	curl_global_cleanup();

	return(1);
}

int App::calcJitterCorrectionOffset()
{
	return 1; //! @todo
}

void App::updateThreadMain()
{
	try
	{
		updateThreadObj = std::make_unique<UpdateThread>();
		updateThreadObj->run();
		updateThreadObj.reset();
	}
	catch(const std::runtime_error &ex)
	{
		std::cout << "Update thread runtime error: " << ex.what() << std::endl;
		stop = true;
	}
}

void App::startUpdateThread()
{
	updateThread = std::thread(std::bind(&App::updateThreadMain, this));
}

void App::stopUpdateThread()
{
	updateThreadObj->stopThread();
	updateThread.join();
}

std::unique_ptr<MySQL_DB> App::createMySQLConnection()
{
	return(std::make_unique<MySQL_DB>(config->get("mysql_host"),
						config->get("mysql_user"),
						config->get("mysql_pass"),
						config->get("mysql_db"),
						config->get("mysql_sock")));
}
