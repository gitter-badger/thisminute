#pragma once

#include <string>
#include <regex>
#include <fstream>
#include <algorithm>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <cassert>
#include <cmath>
#include <vector>
#include <iostream>
#include <vector>
#include <memory>
#include <utility>
#include <thread>
#include <chrono>
#include <mutex>
#include <queue>
#include <ctime>
#include <iterator>
#include <unistd.h>

#include "mysql_connection.h"

#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>

#include "INIReader.h"
#include "timer.h"

using namespace std;

struct Distances
{
	bool enough_data;
	double optics;
	double kondrashov;
	double levenshtein;
	Distances() : enough_data(true), optics(0), levenshtein(0) {};
};

struct Tweet
{
	static Tweet* delimiter;

	bool important = false;

	bool require_update = true;
	double core_distance, smallest_reachability_distance;

	double lat, lon;
	unsigned int x, y, time;
	unordered_set<string> words;
	string text, clean_text, user;
	bool exact;
	multimap<double, Tweet*> optics_neighbors;
	unordered_map<Tweet*, Distances> optics_distances;
	unordered_map<string, double> regional_word_rates;

	Tweet(string _time, string _lat, string _lon, string _text, string _user, string _exact);
	~Tweet();

	bool discern(const Tweet &other_tweet);
};

struct Cell
{
	static vector<vector<Cell>> cells;

	unsigned int tweet_count = 0, x, y;
	unordered_map<string, unordered_set<Tweet*>> tweets_by_word;
	vector<Cell*> region;
};

// utility functions
unordered_set<string> explode(string const &s);
template<typename T> void getArg(T &arg, string section, string option);

// YEAH LET'S DO IT
void Initialize();
void updateTweets(deque<Tweet*> &tweets);
Distances getDistances(const Tweet &a, const Tweet &b);
vector<vector<Tweet*>> getClusters(const deque<Tweet*> &tweets);
void filterClusters(vector<vector<Tweet*>> &clusters);
void writeClusters(vector<vector<Tweet*>> &clusters);
void updateLastRun();
