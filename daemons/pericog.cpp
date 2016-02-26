#include <string>
#include <regex>
#include <fstream>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <cassert>
#include <cmath>
#include <vector>

#include "mysql_connection.h"

#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>

#include "INIReader.h"

#include <array>
#include <vector>
#include <map>
#include <memory>
#include <utility>
#include <thread>
#include <mutex>
#include <queue>

namespace sql {
	class Connection;
	class Driver;
	class ResultSet;
}

using namespace std;

template<class T>
using Grid = vector<vector<T>>;

struct Tweet
{
	Tweet(int x, int y, string text) :
		x(x), y(y), text(text) {}

	int x, y;
	string text;
};

int LOOKBACK_TIME = -1, RECALL_SCOPE, PERIOD, PERIODS_IN_HISTORY;
int MAP_HEIGHT, MAP_WIDTH;
bool HISTORIC_MODE = false;

double WEST_BOUNDARY, EAST_BOUNDARY, NORTH_BOUNDARY, SOUTH_BOUNDARY, RESOLUTION, SPACIAL_PERCENTAGE_THRESHOLD, TEMPORAL_PERCENTAGE_THRESHOLD, SPACIAL_DEVIATIONS_THRESHOLD, TEMPORAL_DEVIATIONS_THRESHOLD;

const int THREAD_COUNT = 8;

sql::Connection* connection;

unordered_set<string> explode(string const &s);
template<typename T> Grid<T> makeGrid();
template<typename T> void getArg(T &arg, string section, string option);

// write updated historic rates to database
string sqlAppendRates(string word, vector<vector<double>> &wordRates);

void commitRates(string sqlValuesString);

unordered_map<int, Tweet> getUserIdToTweetMap();

// refine each tweet into usable information
Grid<int> refineTweetsAndGetTweetCountPerCell(unordered_map<int, Tweet> &userIdTweetMap);

// load the number of times each word was used in every cell
unordered_map <string, Grid<int>> getCurrentWordCountPerCell(const unordered_map<int, Tweet> &userIdTweetMap);

// load historic word usage rates per cell
pair<unordered_map<string, Grid<double>>, unordered_map<string, Grid<double>>> getHistoricWordRatesAndDeviation();

pair<Grid<double>, double> getCurrentLocalAndGlobalRatesForWord(const Grid<int> &wordCountPerCell, const Grid<int> &tweetCountPerCell);

void detectEvents(
	unordered_map<string, Grid<int>> &currentWordCountPerCell,
	unordered_map<string, Grid<double>>    &historicWordRatePerCell,
	unordered_map<string, Grid<double>>    &historicDeviationByCell,
	Grid<int>                        &tweetCountPerCell);

Grid<double> gaussBlur(const Grid<double> &unblurred_array);

void Initialize(int argc, char* argv[])
{
	getArg(RECALL_SCOPE,                  "timing",    "history");
	getArg(PERIOD,                        "timing",    "period");
	getArg(WEST_BOUNDARY,                 "grid",      "west");
	getArg(EAST_BOUNDARY,                 "grid",      "east");
	getArg(SOUTH_BOUNDARY,                "grid",      "south");
	getArg(NORTH_BOUNDARY,                "grid",      "north");
	getArg(RESOLUTION,                    "grid",      "cell_size");
	getArg(SPACIAL_PERCENTAGE_THRESHOLD,  "threshold", "spacial_percentage");
	getArg(TEMPORAL_PERCENTAGE_THRESHOLD, "threshold", "temporal_percentage");
	getArg(SPACIAL_DEVIATIONS_THRESHOLD,  "threshold", "spacial_deviations");
	getArg(TEMPORAL_DEVIATIONS_THRESHOLD, "threshold", "temporal_deviations");

	char tmp;
	while ((tmp = getopt(argc, argv, "l:1:2:3:4:H")) != -1)
	{
		switch (tmp)
		{
			case 'l':
				LOOKBACK_TIME = stoi(optarg);
				break;
			case '1':
				SPACIAL_PERCENTAGE_THRESHOLD = stod(optarg);
				break;
			case '2':
				TEMPORAL_PERCENTAGE_THRESHOLD = stod(optarg);
				break;
			case '3':
				SPACIAL_DEVIATIONS_THRESHOLD = stod(optarg);
				break;
			case '4':
				TEMPORAL_DEVIATIONS_THRESHOLD = stod(optarg);
				break;
			case 'H':
				HISTORIC_MODE = true;
				break;
		}
	}
	assert(LOOKBACK_TIME != -1);

	MAP_WIDTH = static_cast<int>(round(abs((WEST_BOUNDARY - EAST_BOUNDARY) / RESOLUTION)));
	MAP_HEIGHT = static_cast<int>(round(abs((SOUTH_BOUNDARY - NORTH_BOUNDARY) / RESOLUTION)));
	PERIODS_IN_HISTORY = RECALL_SCOPE / PERIOD;
}

int main(int argc, char* argv[])
{
	Initialize(argc, argv);

	// create a connection
	sql::Driver* driver(get_driver_instance());
	{
		ifstream passwordFile("/srv/etc/auth/daemons/pericog.pw");
		auto password = static_cast<ostringstream&>(ostringstream{} << passwordFile.rdbuf()).str();
		connection = driver->connect("tcp://127.0.0.1:3306", "pericog", password);
	}

	// save all tweets since the specified time to an array
	auto userIdToTweetMap = getUserIdToTweetMap();

	auto tweetCountPerCell = refineTweetsAndGetTweetCountPerCell(userIdToTweetMap);

	auto currentWordCountPerCell = getCurrentWordCountPerCell(userIdToTweetMap);

	string query = "INSERT INTO NYC.words_seen (last_seen,word) VALUES ";
	for (const auto &pair : currentWordCountPerCell)
	{
		query += "(FROM_UNIXTIME(" + to_string(LOOKBACK_TIME) + "),'" + pair.first + "'),";
	}
	query.pop_back(); // take the extra comma out

	if (HISTORIC_MODE)
		query += " ON DUPLICATE KEY IGNORE;";
	else
		query += " ON DUPLICATE KEY UPDATE last_seen=FROM_UNIXTIME(" + to_string(LOOKBACK_TIME) + ");";

	connection->createStatement()->execute(query);

	unordered_map<string, Grid<double>> historicWordRatePerCell, historicDeviationByCell;
	tie(historicWordRatePerCell, historicDeviationByCell) = getHistoricWordRatesAndDeviation();

	// consider historic rates that are no longer in use as being currently in use at a rate of 0
	for (const auto &pair : historicWordRatePerCell)
	{
		const auto& word = pair.first;
		if (!currentWordCountPerCell.count(word))
			currentWordCountPerCell[word] = makeGrid<int>();
	}

	string sqlValuesString = "";
	for (const auto &pair : currentWordCountPerCell)
	{
		const auto& word = pair.first;
		const auto &currentCountByCell = pair.second;

		Grid<double> localWordRateByCell;
		double globalWordRate;

		tie(localWordRateByCell, globalWordRate) = getCurrentLocalAndGlobalRatesForWord(currentCountByCell, tweetCountPerCell);

		detectEvents(currentWordCountPerCell, historicWordRatePerCell, historicDeviationByCell, tweetCountPerCell);

		sqlValuesString += sqlAppendRates(word, localWordRateByCell);
	}
	commitRates(sqlValuesString);
	return 0;
}

// write updated historic rates to database
string sqlAppendRates(string word, Grid<double> &wordRates)
{
	string row = "('" + word + "',FROM_UNIXTIME(" + to_string(LOOKBACK_TIME) + "),";
	for (int i = 0; i < MAP_WIDTH*MAP_HEIGHT; i++)
	{
		row += to_string(wordRates[i%MAP_WIDTH][(i-(i%MAP_WIDTH))/MAP_WIDTH]) + ",";
	}
	row.pop_back(); // take the extra comma out
	return row;
}

void commitRates(string sqlValuesString)
{
	string query = "INSERT INTO NYC.rates (word,time,";
	for (int i = 0; i < MAP_WIDTH*MAP_HEIGHT; i++)
	{
		query += "`" + to_string(i) + "`,";
	}
	query.pop_back(); // take the extra comma out
	query += ") VALUES " + sqlValuesString + " ON DUPLICATE KEY IGNORE;";
	connection->createStatement()->execute(query);
}

unordered_set<string> explode(string const &s)
{
	unordered_set<string> result;
	istringstream iss(s);

	for (string token; getline(iss, token, ' '); )
	{
		transform(token.begin(), token.end(), token.begin(), ::tolower);
		result.insert(token);
	}

	return result;
}

template<typename T> Grid<T> makeGrid()
{
	Grid<T> grid(MAP_WIDTH);
	for (int i = 0; i < MAP_WIDTH; i++)
		grid[i] = vector<T>(MAP_HEIGHT);

	return grid;
}

template<typename T> void getArg(T &arg, string section, string option)
{
	static INIReader reader("/srv/etc/config/daemons.ini");
	static double errorValue = -9999;
	arg = (T)reader.GetReal(section, option, errorValue);
	assert(arg != errorValue);
}

unordered_map<int, Tweet> getUserIdToTweetMap()
{
	unordered_map<int, Tweet> tweets;

	unique_ptr<sql::ResultSet> dbTweets(connection->createStatement()->executeQuery(
		"SELECT * FROM NYC.tweets WHERE time > FROM_UNIXTIME(" + to_string(LOOKBACK_TIME) + ");")
		);

	while (dbTweets->next())
	{
		const double lon = stod(dbTweets->getString("lon"));
		const double lat = stod(dbTweets->getString("lat"));

		// if a tweet is located outside of the grid, ignore it and go to the next tweet
		if (lon < WEST_BOUNDARY || lon > EAST_BOUNDARY
			|| lat < SOUTH_BOUNDARY || lat > NORTH_BOUNDARY)
			continue;

		int userId = stoi(dbTweets->getString("user"));

		auto tweetIter = tweets.find(userId);
		if (tweetIter == tweets.end())
		{
			const int
				x = floor((lon - WEST_BOUNDARY) / RESOLUTION),
				y = floor((lat - SOUTH_BOUNDARY) / RESOLUTION);

			Tweet tweet(x, y, dbTweets->getString("text"));

			tweets.insert({ userId, tweet });
		}
		else
		{
			tweetIter->second.text += " " + dbTweets->getString("text");
		}
	}

	return tweets;
}

// refine each tweet into usable information
Grid<int> refineTweetsAndGetTweetCountPerCell(unordered_map<int, Tweet> &userIdTweetMap)
{
	unordered_map<string, Grid<int>> wordCountPerCell;
	auto tweetsPerCell = makeGrid<int>();

	queue<Tweet*> tweetQueue;
	for (auto &pair : userIdTweetMap)
		tweetQueue.push(&pair.second);

	mutex tweetQueueLock;
	mutex tweetsPerCellLock;

	auto processTweets = [&]()
		{
			while (true)
			{
				tweetQueueLock.lock();
				if (tweetQueue.empty())
				{
					tweetQueueLock.unlock();
					return;
				}
				Tweet &tweet = *tweetQueue.front();
				tweetQueue.pop();
				tweetQueueLock.unlock();

				// remove mentions and URLs
				regex mentionsAndUrls("((\\B@)|(\\bhttps?:\\/\\/))[^\\s]+");
				tweet.text = regex_replace(tweet.text, mentionsAndUrls, string(" "));

				// remove all non-word characters
				regex nonWord("[^\\w]+");
				tweet.text = regex_replace(tweet.text, nonWord, string(" "));

				tweetsPerCellLock.lock();
				tweetsPerCell[tweet.x][tweet.y]++;
				tweetsPerCellLock.unlock();
			}
		};

	vector<thread> threads;
	for (int i = 0; i < THREAD_COUNT; i++)
		threads.emplace_back(processTweets);

	for (int i = 0; i < THREAD_COUNT; i++)
		threads[i].join();

	return tweetsPerCell;
}

// refine each tweet into usable information
unordered_map <string, Grid<int>> getCurrentWordCountPerCell(const unordered_map<int, Tweet> &userIdTweetMap)
{
	unordered_map<string, Grid<int>> wordCountPerCell;

	for (const auto &pair : userIdTweetMap)
	{
		auto tweet = pair.second;

		auto words = explode(tweet.text);
		for (const auto &word : words)
		{
			if (!wordCountPerCell.count(word))
				wordCountPerCell[word] = makeGrid<int>();

			wordCountPerCell[word][tweet.x][tweet.y]++;
		}
	}

	// the '&' character is interpreted as the word "amp"... squelch for now
	wordCountPerCell.erase("amp");

	// the word ' ' shows up sometimes... squelch for now
	wordCountPerCell.erase(" ");

	return wordCountPerCell;
}

pair<unordered_map<string, Grid<double>>, unordered_map<string, Grid<double>>> getHistoricWordRatesAndDeviation()
{
	unordered_map<string, Grid<double>> historicWordRates, historicDeviations;

	unique_ptr<sql::ResultSet> dbWordsSeen(connection->createStatement()->executeQuery(
		"SELECT * FROM NYC.words_seen;"
		));

	while (dbWordsSeen->next())
	{
		const auto word = dbWordsSeen->getString("word");
		unique_ptr<sql::ResultSet> wordRates(connection->createStatement()->executeQuery(
			"SELECT * FROM NYC.rates WHERE word ='" + word + "' AND time BETWEEN FROM_UNIXTIME(" +
			to_string(LOOKBACK_TIME) + ") AND FROM_UNIXTIME(" + to_string(LOOKBACK_TIME - RECALL_SCOPE) + ") ORDER BY time DESC;"
			));

		historicWordRates[word] = makeGrid<double>();
		historicDeviations[word] = makeGrid<double>();
		vector<Grid<double>> rates;

		while (wordRates->next())
		{
			rates.push_back(makeGrid<double>());
			for (int i = 0; i < MAP_WIDTH; i++)
			{
				for (int j = 0; j < MAP_HEIGHT; j++)
				{
					rates.back()[i][j] = stod(wordRates->getString('`' + to_string(j*MAP_WIDTH + i) + '`'));
				}
			}
		}

		for (int i = 0; i < MAP_WIDTH; i++)
		{
			for (int j = 0; j < MAP_HEIGHT; j++)
			{
				for (const auto &rate : rates)
					historicWordRates[word][i][j] += rate[i][j];
				historicWordRates[word][i][j] /= PERIODS_IN_HISTORY;

				for (const auto &rate : rates)
					historicDeviations[word][i][j] += pow(rate[i][j] - historicWordRates[word][i][j], 2);
				historicDeviations[word][i][j] /= PERIODS_IN_HISTORY;

				historicDeviations[word][i][j] = pow(historicDeviations[word][i][j], 0.5);
			}
		}
	}

	return{ move(historicWordRates), move(historicDeviations) };
}

// calculate the usage rate of each word at the current time in each cell and the average regional use
pair<Grid<double>, double> getCurrentLocalAndGlobalRatesForWord(const Grid<int> &wordCountPerCell, const Grid<int> &tweetCountPerCell)
{
	Grid<double> localWordRates = makeGrid<double>();
	double globalWordRate = 0;
	int totalTweets = 0;
	for (int i = 0; i < MAP_WIDTH; i++)
	{
		for (int j = 0; j < MAP_HEIGHT; j++)
		{
			if (tweetCountPerCell[i][j])
			{
				localWordRates[i][j] = (double)wordCountPerCell[i][j]/tweetCountPerCell[i][j];
				globalWordRate += wordCountPerCell[i][j];
				totalTweets += tweetCountPerCell[i][j];
			}
			else
			{
				localWordRates[i][j] = 0;
			}
		}
	}
	globalWordRate /= totalTweets;

	return{ move(localWordRates), globalWordRate };
}

// returns pointer to a gaussian blurred 2d array with given dimensions
Grid<double> gaussBlur(const Grid<double> &unblurred_array)
{
	static const double gaussValueMatrix[3] = { 0.22508352, 0.11098164, 0.05472157 }; // mid, perp, diag

	auto& width = MAP_WIDTH;
	auto& height = MAP_HEIGHT;

	// declare a new 2d array to store the blurred values
	auto blurred_array = makeGrid<double>();

	// for each value in the unblurred array, sum the products of that value and each value in the gaussValueMatrix

	for (int j = 0; j < height; j++)
	{
		for (int i = 0; i < width; i++)
		{
			bool left_bound = i == 0, right_bound = i == (width - 1);
			bool top_bound = j == 0, bottom_bound = j == (height - 1);

			// blur the middle
			blurred_array[i][j] += unblurred_array[i][j] * gaussValueMatrix[0];

			if (!left_bound)
			{
				// blur the middle left
				blurred_array[i][j] += unblurred_array[i - 1][j] * gaussValueMatrix[1];

				if (!top_bound)
				{
					//blur the top left
					blurred_array[i][j] += unblurred_array[i - 1][j - 1] * gaussValueMatrix[2];
				}
				if (!bottom_bound)
				{
					// blur the bottom left
					blurred_array[i][j] += unblurred_array[i - 1][j + 1] * gaussValueMatrix[2];
				}
			}

			if (!right_bound)
			{
				// blur the middle right
				blurred_array[i][j] += unblurred_array[i + 1][j] * gaussValueMatrix[1];

				if (!top_bound)
				{
					// blur the top right
					blurred_array[i][j] += unblurred_array[i + 1][j - 1] * gaussValueMatrix[2];
				}
				if (!bottom_bound)
				{
					// blur the bottom right
					blurred_array[i][j] += unblurred_array[i + 1][j + 1] * gaussValueMatrix[2];
				}
			}

			if (!top_bound)
			{
				// blur the top middle
				blurred_array[i][j] += unblurred_array[i][j - 1] * gaussValueMatrix[1];
			}

			if (!bottom_bound)
			{
				// blur the bottom middle
				blurred_array[i][j] += unblurred_array[i][j + 1] * gaussValueMatrix[1];
			}
		}
	}

	return blurred_array;
}

void detectEvents(
	unordered_map<string, Grid<int>>    &currentWordCountPerCell,
	unordered_map<string, Grid<double>> &historicWordRatePerCell,
	unordered_map<string, Grid<double>> &historicDeviationByCell,
	Grid<int>                           &tweetCountPerCell
)
{
	mutex listLock;
	queue<string> wordQueue;
	for (auto &pair : currentWordCountPerCell)
	{
		wordQueue.push(pair.first);
	}

	auto detectEventForWord = [&]()
		{
			while (true)
			{
				listLock.lock();
				if (wordQueue.empty())
				{
					listLock.unlock();
					return;
				}
				string &word = wordQueue.front();
				wordQueue.pop();
				listLock.unlock();

				// calculate the usage rate of each word at the current time in each cell and the average regional use
				Grid<double> localWordRateByCell;
				double globalWordRate;
				tie(localWordRateByCell, globalWordRate) = getCurrentLocalAndGlobalRatesForWord(currentWordCountPerCell[word], tweetCountPerCell);

				double globalDeviation = 0;
				for (int i = 0; i < MAP_WIDTH; i++)
				{
					for (int j = 0; j < MAP_HEIGHT; j++)
					{
						globalDeviation += pow(localWordRateByCell[i][j] - globalWordRate, 2);
					}
				}
				globalDeviation /= MAP_WIDTH * MAP_HEIGHT;
				globalDeviation = pow(globalDeviation, 0.5);

				// blur the rates over cell borders to reduce noise
				localWordRateByCell = gaussBlur(localWordRateByCell);

				// detect events!! and adjust historic rates
				for (int i = 0; i < MAP_WIDTH; i++)
				{
					for (int j = 0; j < MAP_HEIGHT; j++)
					{
						if (
							// checks if a word is a appearing with a greater percentage in one cell than in other cells in the city grid
							(localWordRateByCell[i][j] > globalWordRate + SPACIAL_PERCENTAGE_THRESHOLD) &&
							// checks if a word is appearing more frequently in a cell than it has historically in that cell
							(localWordRateByCell[i][j] > historicWordRatePerCell[word][i][j] + TEMPORAL_PERCENTAGE_THRESHOLD) &&
							(localWordRateByCell[i][j] > historicWordRatePerCell[word][i][j] + globalDeviation * SPACIAL_DEVIATIONS_THRESHOLD) &&
							(localWordRateByCell[i][j] > historicWordRatePerCell[word][i][j] + historicDeviationByCell[word][i][j] * TEMPORAL_DEVIATIONS_THRESHOLD)
							)
						{
							if (!HISTORIC_MODE)
							{
								connection->createStatement()->execute(
									"INSERT INTO NYC.events (word, x, y) VALUES ('" + word + "'," + to_string(i) + "," + to_string(j) + ");"
									);
							}
							{
								ofstream resultFile;
								resultFile.open ("/root/test/" + to_string(LOOKBACK_TIME) + ".txt", std::ofstream::out | std::ofstream::app);
								resultFile << word + "\n";
								resultFile.close();
							}
						}
					}
				}
			}
		};

	vector<thread> threads;
	for (int i = 0; i < THREAD_COUNT; i++)
	{
		threads.emplace_back(detectEventForWord);
	}

	for (int i = 0; i < THREAD_COUNT; i++)
	{
		threads[i].join();
	}
}