/*
 * ClauseDatabase.h
 *
 *  Created on: Aug 27, 2014
 *      Author: balyo
 */

#ifndef CLAUSEDATABASE_H_
#define CLAUSEDATABASE_H_

#include <vector>

#include "Threading.h"
#include "logging_interface.h"
#include "default_logging_interface.h"

using namespace std;

#define BUCKET_SIZE 1000

struct Bucket {
	int data[BUCKET_SIZE];
	unsigned int top;
};

class ClauseDatabase {
public:
	ClauseDatabase() : logger(dli) {}
	ClauseDatabase(LoggingInterface& logger) : logger(logger) {}
	virtual ~ClauseDatabase();

	/**
	 * Add a learned clause that you want to share. Return a pointer to it
	 */
	int* addClause(vector<int>& clause);
	/**
	 * Add a very important learned clause that you want to share
	 */
	void addVIPClause(vector<int>& clause);
	/**
	 * Fill the given buffer with data for the sending our learned clauses
	 * Return the number of used memory, at most size.
	 */
	unsigned int giveSelection(int* buffer, unsigned int size, int* selectedCount = NULL);
	/**
	 * Set the pointer for the buffer of <size> containing the incoming shared clauses 
	 * which has the same shape as the data returned by the giveSelection method.
	 */
	void setIncomingBuffer(const int* buffer, int size);
	/**
	 * Fill the given clause with the literals of the next incomming clause.
	 * Return false if no more clauses.
	 */
	bool getNextIncomingClause(vector<int>& clause);

private:
	static DefaultLoggingInterface dli;
	LoggingInterface& logger;
	Mutex addClauseLock;

	// Structures for EXPORTING
	vector<Bucket*> buckets;
	vector<vector<int> > vipClauses;

	// Structures for IMPORTING	
	const int* incomingBuffer;
	unsigned int bufferSize;
	int currentPos;
	int currentSize; // 0 for VIP clauses
	int remainingVipLits;
	int remainingClsOfCurrentSize;

};

#endif /* CLAUSEDATABASE_H_ */