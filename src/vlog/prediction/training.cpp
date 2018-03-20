#include <vlog/training.h>
#include <csignal>
#include <ctime>
#include <stack>
#include <numeric>

std::string makeGenericQuery(Program& p, PredId_t predId, uint8_t predCard) {
    std::string query = p.getPredicateName(predId);
    query += "(";
    for (int i = 0; i < predCard; ++i) {
        query += "V" + to_string(i+1);
        if (i != predCard-1) {
            query += ",";
        }
    }
    query += ")";
    return query;
}
// http://www.cplusplus.com/forum/general/125094/
std::vector<std::string> split( std::string str, char sep = ' ' )
{
    std::vector<std::string> ret ;

    std::istringstream stm(str) ;
    std::string token ;
    while( std::getline( stm, token, sep ) ) ret.push_back(token) ;

    return ret ;
}

std::string stringJoin(vector<string>& vec, char delimiter=','){
    string result;
    for (int i = 0; i < vec.size(); ++i) {
        result += vec[i];
        if (i != vec.size()-1) {
            result += delimiter;
        }
    }
    return result;
}

std::pair<std::string, int> makeComplexQuery(Program& p, Literal& l, vector<Substitution>& sub, EDBLayer& db) {
    std::string query = p.getPredicateName(l.getPredicate().getId());
    int card = l.getPredicate().getCardinality();
    query += "(";
    QueryType queryType;
    int countConst = 0;
    for (int i = 0; i < card; ++i) {
        std::string canV = "V" + to_string(i+1);
        //FIXME: uint8_t id = p.getIDVar(canV); //I don't know how to convert this line
        uint8_t id = 0;
        bool found = false;
        for (int j = 0; j < sub.size(); ++j) {
            if (sub[j].origin == id) {
                char supportText[MAX_TERM_SIZE];
                db.getDictText(sub[j].destination.getValue(), supportText);
                query += supportText;
                found = true;
                countConst++;
            }
        }
        if (!found) {
            query += canV;
        }
        if (i != card-1) {
            query += ",";
        }
    }
    query += ")";

    if (countConst == card) {
        queryType = QUERY_TYPE_BOOLEAN;
    } else if (countConst == 0) {
        queryType = QUERY_TYPE_GENERIC;
    } else {
        queryType = QUERY_TYPE_MIXED;
    }
    return std::make_pair(query, queryType);
}

template <typename Generic>
std::vector<std::vector<Generic>> powerset(std::vector<Generic>& set) {
    std::vector<std::vector<Generic>> output;
    uint16_t setSize = set.size();
    uint16_t powersetSize = pow((uint16_t)2, setSize) - 1;
    for (int i = 1; i <= powersetSize; ++i) {
        std::vector<Generic> element;
        for (int j = 0; j < setSize; ++j) {
            if (i & (1<<j)) {
                element.push_back(set[j]);
            }
        }
        output.push_back(element);
    }
    return output;
}

PredId_t getMatchingIDB(EDBLayer& db, Program &p, vector<uint64_t>& tuple) {
    //Check this tuple with all rules
    PredId_t idbPredicateId = 65535;
    vector<Rule> rules = p.getAllRules();
    vector<Rule>::iterator it = rules.begin();
    vector<pair<uint8_t, uint64_t>> ruleTuple;
    for (;it != rules.end(); ++it) {
        vector<Literal> body = (*it).getBody();
        if (body.size() > 1) {
            continue;
        }
        uint8_t nConstants = body[0].getNConstants();
        Predicate temp = body[0].getPredicate();
        if (!p.isPredicateIDB(temp.getId())){
            int matched = 0;
            for (int c = 0; c < temp.getCardinality(); ++c) {
                uint8_t tempid = body[0].getTermAtPos(c).getId();
                if(tempid == 0) {
                    uint64_t tempvalue = body[0].getTermAtPos(c).getValue();
                    char supportText[MAX_TERM_SIZE];
                    db.getDictText(tempvalue, supportText);
                    if (tempvalue == tuple[c]) {
                        matched++;
                    }
                }
            }
            if (matched == nConstants) {
                idbPredicateId = (*it).getFirstHead().getPredicate().getId();
                return idbPredicateId;
            }
        }
    }
    return idbPredicateId;
}

void getRandomTupleIndexes(uint64_t m, uint64_t n, vector<int>& indexes) {
    srand(time(0));
    for (uint64_t i = 0; i < m; ++i) {
        uint64_t r;
        do {
            r = rand() % n;
        } while(std::find(indexes.begin(), indexes.end(), r) != indexes.end());
        indexes[i] = r;
    }
}

bool isSimilar(string& query1, string& query2, EDBLayer& layer) {
    vector<string> tokens1 = split(query1, '(');
    vector<string> tokens2 = split(query2, '(');

    string predicate1 = tokens1[0];
    string predicate2 = tokens2[0];
    if (predicate1 != predicate2) {
        return false;
    }
    string tuple1 = tokens1[1];
    string tuple2 = tokens2[1];
    // Remove trailing closing paranthesis (')')
    tuple1.pop_back();
    tuple2.pop_back();
    vector<string> terms1 = split(tuple1, ',');
    vector<string> terms2 = split(tuple2, ',');
    if (terms1.size() != terms2.size()) {
        return false;
    }
    for (int i = 0; i < terms1.size(); ++i){
        uint64_t value1;
        bool isConstant1 = layer.getDictNumber((char*) terms1[i].c_str(), terms1[i].size(), value1);
        uint64_t value2;
        bool isConstant2 = layer.getDictNumber((char*) terms2[i].c_str(), terms2[i].size(), value2);
        if (!isConstant1 && isConstant2) {
            return false;
        }
    }
    return true;
}

int foundSubsumingQuery(string& testQuery,
    vector<pair<string, int>>& trainingQueriesAndResult,
    Program& p,
    EDBLayer& layer) {

    Dictionary dictVariables;
    Literal testLiteral = p.parseLiteral(testQuery, dictVariables);
    for (auto qr: trainingQueriesAndResult) {
        // If the test query has occurred in the training, then return the result of that query
        if (testQuery == qr.first) {
            LOG(INFOL) << testQuery << " same as " << qr.first;
            return qr.second;
        }

        // If the test query is similar to the training query, then return the result of that query
        if (isSimilar(testQuery, qr.first, layer)){
            LOG(INFOL) << testQuery << " similar to " << qr.first;
            return qr.second;
        }
        //Literal trainingLiteral = p.parseLiteral(qr.first, dictVariables);
        //Substitution subs[SIZETUPLE];
        //int nsubs = Literal::subsumes(subs, testLiteral, trainingLiteral);
        //if (nsubs != -1) {
        //    LOG(INFOL) << testQuery << " subsumes " << qr.first;
            // return result of the test query
            //LOG(INFOL) << "returning " << qr.second;
        //    return qr.second;
        //}
    }
    return -1;
}

std::vector<std::pair<std::string, int>> Training::generateTrainingQueries(EDBConf &conf,
        EDBLayer &db,
        Program &p,
        int depth,
        uint64_t maxTuples,
        std::vector<uint8_t>& vt
        ) {
    std::unordered_map<string, int> allQueries;

    typedef std::pair<PredId_t, vector<Substitution>> EndpointWithEdge;
    typedef std::unordered_map<uint16_t, std::vector<EndpointWithEdge>> Graph;
    Graph graph;

    std::vector<Rule> rules = p.getAllRules();
    for (int i = 0; i < rules.size(); ++i) {
        Rule ri = rules[i];
        Predicate ph = ri.getFirstHead().getPredicate();
        std::vector<Substitution> sigmaH;
        for (int j = 0; j < ph.getCardinality(); ++j) {
            VTerm dest = ri.getFirstHead().getTuple().get(j);
            sigmaH.push_back(Substitution(vt[j], dest));
        }
        std::vector<Literal> body = ri.getBody();
        for (std::vector<Literal>::const_iterator itr = body.begin(); itr != body.end(); ++itr) {
            Predicate pb = itr->getPredicate();
            std::vector<Substitution> sigmaB;
            for (int j = 0; j < pb.getCardinality(); ++j) {
                VTerm dest = itr->getTuple().get(j);
                sigmaB.push_back(Substitution(vt[j], dest));
            }
            // Calculate sigmaB * sigmaH
            std::vector<Substitution> edge_label = inverse_concat(sigmaB, sigmaH);
            EndpointWithEdge neighbour = std::make_pair(ph.getId(), edge_label);
            graph[pb.getId()].push_back(neighbour);
        }
    }

#if DEBUG
    // Try printing graph
    for (auto it = graph.begin(); it != graph.end(); ++it) {
        uint16_t id = it->first;
        std::cout << p.getPredicateName(id) << " : " << std::endl;
        std::vector<EndpointWithEdge> nei = it->second;
        for (int i = 0; i < nei.size(); ++i) {
            Predicate pred = p.getPredicate(nei[i].first);
            std::vector<Substitution> sub = nei[i].second;
            for (int j = 0; j < sub.size(); ++j){
                std::cout << p.getPredicateName(nei[i].first) << "{" << sub[j].origin << "->"
                    << sub[j].destination.getId() << " , " << sub[j].destination.getValue() << "}" << std::endl;
            }
        }
        std::cout << "=====" << std::endl;
    }
#endif

    // Gather all predicates
    std::vector<PredId_t> ids = p.getAllEDBPredicateIds();
    std::ofstream allPredicatesLog("allPredicatesInQueries.log");
    Dictionary dictVariables;
    for (int i = 0; i < ids.size(); ++i) {
        int neighbours = graph[ids[i]].size();
        LOG(INFOL) << p.getPredicateName(ids[i]) << " is EDB : " << neighbours << "neighbours";
        Predicate edbPred = p.getPredicate(ids[i]);
        int card = edbPred.getCardinality();
        std::string query = makeGenericQuery(p, edbPred.getId(), edbPred.getCardinality());
        Literal literal = p.parseLiteral(query, dictVariables);
        int nVars = literal.getNVars();
        QSQQuery qsqQuery(literal);
        TupleTable *table = new TupleTable(nVars);
        db.query(&qsqQuery, table, NULL, NULL);
        uint64_t nRows = table->getNRows();
        std::vector<std::vector<uint64_t>> output;
        /**
         * RP1(A,B) :- TE(A, <studies>, B)
         * RP2(A,B) :- TE(A, <worksFor>, B)
         *
         * Tuple <jon, studies, VU> can match with RP2 (because TE has RP1 and RP2 both as neighbours)
         * , but it should not match
         *
         * All EDB tuples should be carefully matched with rules
         * */
        PredId_t predId = edbPred.getId();
        vector<int> tupleIndexes(maxTuples);
        getRandomTupleIndexes(maxTuples, nRows, tupleIndexes);

        uint64_t rowNumber = 0;
        if (maxTuples > nRows) {
            maxTuples = nRows;
        }
        while (rowNumber < maxTuples) {
            std::vector<uint64_t> tuple;
            std::string tupleString("<");
            for (int j = 0; j < nVars; ++j) {
                uint64_t value = table->getPosAtRow(tupleIndexes[rowNumber], j);
                tuple.push_back(value);
                char supportText[MAX_TERM_SIZE];
                db.getDictText(value, supportText);
                tupleString += supportText;
                tupleString += ",";
            }
            tupleString += ">";
            PredId_t idbPredId = getMatchingIDB(db, p, tuple);
            if (65535 == idbPredId) {
                rowNumber++;
                continue;
            }
            std::string predName = p.getPredicateName(idbPredId);

            LOG(INFOL) << "Matched : " << tupleString << " ==> " << predName << " : " << +idbPredId;
            vector<Substitution> subs;
            for (int k = 0; k < card; ++k) {
                subs.push_back(Substitution(vt[k], VTerm(0, tuple[k])));
            }
            // Find powerset of subs here
            std::vector<std::vector<Substitution>> options =  powerset<Substitution>(subs);
            //unsigned int seed = (unsigned int) ((clock() ^ 413711) % 105503);
            srand(time(0));
            for (int l = 0; l < options.size(); ++l) {
                vector<Substitution> sigma = options[l];
                PredId_t predId = edbPred.getId();
                int n = 1;
                while (n != depth+1) {
                    uint32_t nNeighbours = graph[predId].size();
                    if (0 == nNeighbours) {
                        break;
                    }
                    uint32_t randomNeighbour;
                    if (1 == n) {
                        int index = 0;
                        bool found = false;
                        for (auto it = graph[predId].begin(); it != graph[predId].end(); ++it,++index) {
                            if (it->first == idbPredId) {
                                randomNeighbour = index;
                                found = true;
                                break;
                            }
                        }
                        assert(found == true);
                    } else {
                        randomNeighbour = rand() % nNeighbours;
                    }
                    std::vector<Substitution>sigmaN = graph[predId][randomNeighbour].second;
                    std::vector<Substitution> result = concat(sigmaN, sigma);
                    PredId_t qId  = graph[predId][randomNeighbour].first;
                    uint8_t qCard = p.getPredicate(graph[predId][randomNeighbour].first).getCardinality();
                    std::string qQuery = makeGenericQuery(p, qId, qCard);
                    Literal qLiteral = p.parseLiteral(qQuery, dictVariables);
                    allPredicatesLog << p.getPredicateName(qId) << std::endl;
                    std::pair<string, int> finalQueryResult = makeComplexQuery(p, qLiteral, result, db);
                    std::string qFinalQuery = finalQueryResult.first;
                    int type = finalQueryResult.second + ((n > 4) ? 4 : n);
                    if (allQueries.find(qFinalQuery) == allQueries.end()) {
                        allQueries.insert(std::make_pair(qFinalQuery, type));
                    }

                    predId = qId;
                    sigma = result;
                    n++;
                } // while the depth of exploration is reached
            } // for each partial substitution
            rowNumber++;
        }
    } // all EDB predicate ids
    allPredicatesLog.close();
    std::vector<std::pair<std::string,int>> queries;
    for (std::unordered_map<std::string,int>::iterator it = allQueries.begin(); it !=  allQueries.end(); ++it) {
        queries.push_back(std::make_pair(it->first, it->second));
        LOG(INFOL) << "Query: " << it->first << " type : " << it->second ;
    }
    return queries;
}
pid_t pid;
bool timedOut;
void alarmHandler(int signalNumber) {
    if (signalNumber == SIGALRM) {
        kill(pid, SIGKILL);
        timedOut = true;
    }
}

double Training::runAlgo(string& algo,
        Reasoner& reasoner,
        EDBLayer& edb,
        Program& p,
        Literal& literal,
        stringstream& ss,
        uint64_t timeoutMillis) {

    int ret;

    std::chrono::duration<double> durationQuery;
    signal(SIGALRM, alarmHandler);
    timedOut = false;

    double* queryTime = (double*) mmap(NULL, sizeof(double), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);

    pid = fork();
    if (pid < 0) {
        LOG(ERRORL) << "Could not fork";
        return 0.0L;
    } else if (pid == 0) {
        //Child work begins
        //+
        std::chrono::system_clock::time_point queryStartTime = std::chrono::system_clock::now();
        bool printResults = false;
        int nVars = literal.getNVars();
        bool onlyVars = nVars > 0;
        TupleIterator *iter;
        if (algo == "magic"){
            iter = reasoner.getMagicIterator(literal, NULL, NULL, edb, p, onlyVars, NULL);
        } else if (algo == "qsqr") {
            iter = reasoner.getTopDownIterator(literal, NULL, NULL, edb, p, onlyVars, NULL);
        } else {
            LOG(ERRORL) << "Algorithm not supported : " << algo;
            return 0;
        }
        long count = 0;
        int sz = iter->getTupleSize();
        if (nVars == 0) {
            ss << (iter->hasNext() ? "TRUE" : "FALSE") << endl;
            count = (iter->hasNext() ? 1 : 0);
        } else {
            while (iter->hasNext()) {
                iter->next();
                count++;
                if (printResults) {
                    for (int i = 0; i < sz; i++) {
                        char supportText[MAX_TERM_SIZE];
                        uint64_t value = iter->getElementAt(i);
                        if (i != 0) {
                            ss << " ";
                        }
                        if (!edb.getDictText(value, supportText)) {
                            LOG(ERRORL) << "Term " << value << " not found";
                        } else {
                            ss << supportText;
                        }
                    }
                    ss << endl;
                }
            }
        }
        std::chrono::system_clock::time_point queryEndTime = std::chrono::system_clock::now();
        durationQuery = queryEndTime - queryStartTime;
        *queryTime = durationQuery.count()*1000;
        exit(0);
        //-
        //Child work ends
    } else {
        uint64_t l =  timeoutMillis / 1000;
        alarm(l);
        int status;
        ret = waitpid(pid, &status, 0);
        alarm(0);
        if (timedOut) {
            LOG(INFOL) << "TIMED OUT";
            munmap(queryTime, sizeof(double));
            return timeoutMillis;
        }
        double time = *queryTime;
        munmap(queryTime, sizeof(double));
        return time;
    }
}

void separateQuery(string logLine, vector<string>& tokens) {
    stack<int> spaceIndexes;
    size_t index = string::npos;
    int cntIndexes = 0;
    while (cntIndexes < 4) {
        index = logLine.find_last_of(" ", index);
        assert(index != string::npos);
        index--;
        spaceIndexes.push(index);
        cntIndexes++;
    }

    int startIndex = 0;
    while(!spaceIndexes.empty()) {
        int index = spaceIndexes.top();
        spaceIndexes.pop();
        tokens.push_back(logLine.substr(startIndex, (index - startIndex)+1));
        startIndex = index+2;
    }
    tokens.push_back(logLine.substr(startIndex, (index - startIndex)+1));
}

void parseQueriesLog(vector<string>& testQueriesLog,
        vector<string>& testQueries,
        vector<Metrics>& testFeaturesVector,
        vector<int>& expectedDecisions) {

    for (auto line: testQueriesLog) {
        // A line looks like this (An underscore (_) represents a space)
        // query_fe,a,t,u,r,es_QSQTime_MagicTime_Decision
        //RP29(<http://www.Department4.University60.edu/FullProfessor5>,B) 4.000000,4,1,1,2,0 1.290000 2.069000 1
        // some queries can contain spaces, commas. To address these, we split tokens by scanning the string from the end
        //RP1052(A,"(A)National Security (B) Public Accounts(C)Rules,Business & Privliges (D) Foreign Affairs (E) Kashmir"@en)
        vector<string> tokens;
        separateQuery(line, tokens);
        assert (tokens.size() == 5);
        vector<string> features = split(tokens[1], ',');
        testQueries.push_back(tokens[0]);
        Metrics metrics;
        metrics.cost = stod(features[0]);
        metrics.estimate = stoul(features[1]);
        metrics.countRules = stoi(features[2]);
        metrics.countUniqueRules = stoi(features[3]);
        metrics.countIntermediateQueries = stoi(features[4]);
        metrics.countIDBPredicates = stoi(features[5]);
        testFeaturesVector.push_back(metrics);
        expectedDecisions.push_back(stoi(tokens[4]));
    }
}

void Training::trainAndTestModel(vector<string>& trainingQueriesVector,
        vector<string>& testQueriesLog,
        EDBLayer& edb,
        Program& p,
        double& accuracy,
        uint64_t timeout,
        uint8_t repeatQuery) {

    vector<Metrics> featuresVector;
    vector<int> decisionVector;
    int i = 1;
    vector<string> strResults;
    vector<string> strFeatures;
    vector<string> strQsqrTime;
    vector<string> strMagicTime;
    ofstream logTrainingMagic("training-magic.log");
    for (auto q : trainingQueriesVector) {
        LOG(INFOL) << i++ << ") " << q;
        // Execute the literal query
        string results="";
        string features="";
        string qsqrTime="";
        string magicTime="";
        Training::execLiteralQuery(q,
                edb,
                p,
                results,
                features,
                qsqrTime,
                magicTime,
                timeout,
                repeatQuery,
                featuresVector,
                decisionVector);
        strResults.push_back(results);
        strFeatures.push_back(features);
        strQsqrTime.push_back(qsqrTime);
        strMagicTime.push_back(magicTime);
    }

    vector<pair<string, int>> trainingQueriesAndResult;

    int trainingQsqr = 0;
    int trainingMagic = 0;
    vector<Instance> dataset;
    for (int i = 0; i < featuresVector.size(); ++i) {
        vector<double> features;
        features.push_back(featuresVector[i].cost);
        features.push_back(featuresVector[i].estimate);
        features.push_back(featuresVector[i].countRules);
        features.push_back(featuresVector[i].countUniqueRules);
        features.push_back(featuresVector[i].countIntermediateQueries);
        features.push_back(featuresVector[i].countIDBPredicates);
        int label = decisionVector[i];
        if (label == 1) {
            trainingQsqr++;
        } else {
            trainingMagic++;
            trainingQueriesAndResult.push_back(std::make_pair(trainingQueriesVector[i], label));
            logTrainingMagic << trainingQueriesVector[i] << endl;
        }
        Instance instance(label, features);
        dataset.push_back(instance);
    }

    if (logTrainingMagic.fail()) {
        LOG(ERRORL) << "Error writing to file";
    }
    logTrainingMagic.close();

    LogisticRegression lr(6);
    lr.train(dataset);

    ofstream logCorrectlyGuessedMagic("correctly-guessed-magic.log");
    vector<Metrics> testMetrics;
    vector<string> testQueries;
    vector<int> testDecisions;
    parseQueriesLog(testQueriesLog, testQueries, testMetrics, testDecisions);
    int hit = 0;
    int totalQsqr = 0;
    int totalMagic = 0;
    int hitQsqr = 0;
    int hitMagic = 0;
    LOG(INFOL) << " # Test queries = " << testMetrics.size();
    for (int i = 0; i < testMetrics.size(); ++i) {
        vector<double> features;
        features.push_back(testMetrics[i].cost);
        features.push_back(testMetrics[i].estimate);
        features.push_back(testMetrics[i].countRules);
        features.push_back(testMetrics[i].countUniqueRules);
        features.push_back(testMetrics[i].countIntermediateQueries);
        features.push_back(testMetrics[i].countIDBPredicates);

        int myDecision = 0;
        double probability = lr.classify(features);
        if (probability > 0.5) {
            myDecision = 1;
        }
        int result = -1;
        if (myDecision != testDecisions[i]) {
            result = foundSubsumingQuery(testQueries[i], trainingQueriesAndResult, p, edb);
        }
        if (result != -1) {
            myDecision = result;
        }

        if (testDecisions[i] == 1) {
            totalQsqr++;
        } else {
            totalMagic++;
        }
        if (myDecision == testDecisions[i]) {
            if (myDecision == 1) {
                hitQsqr++;
            } else {
                hitMagic++;
                logCorrectlyGuessedMagic << testQueries[i] << endl;
            }
            hit++;
        }
    }

    if (logCorrectlyGuessedMagic.fail()) {
        LOG(ERRORL) << "Error writing to file";
    }
    logCorrectlyGuessedMagic.close();

    accuracy = (double)hit/(double)testMetrics.size();
    LOG(INFOL) << "Overall Accuracy : " << accuracy;
    LOG(INFOL) << "QSQR Accuracy : " << hitQsqr << " / " << totalQsqr << " = " <<  (double)hitQsqr/(double)totalQsqr;
    LOG(INFOL) << "Magic Accuracy : " << hitMagic << " / " << totalMagic << " = " <<  (double)hitMagic/(double)totalMagic;
    LOG(INFOL) << "QSQR favouring Training Queries = " << trainingQsqr;
    LOG(INFOL) << "Magic favouring Training Queries = " << trainingMagic;
}

void Training::execLiteralQueries(vector<string>& queryVector,
        EDBLayer& edb,
        Program& p,
        JSON* jsonResults,
        JSON* jsonFeatures,
        JSON* jsonQsqrTime,
        JSON* jsonMagicTime,
        uint64_t timeout,
        uint8_t repeatQuery) {

    vector<Metrics> featuresVector;
    vector<int> decisionVector;
    int i = 1;
    vector<string> strResults;
    vector<string> strFeatures;
    vector<string> strQsqrTime;
    vector<string> strMagicTime;
    ofstream logFile("queries-execution.log");
    for (auto q : queryVector) {
        LOG(INFOL) << i++ << ") " << q;
        // Execute the literal query
        string results="";
        string features="";
        string qsqrTime="";
        string magicTime="";
        Training::execLiteralQuery(q,
                edb,
                p,
                results,
                features,
                qsqrTime,
                magicTime,
                timeout,
                repeatQuery,
                featuresVector,
                decisionVector);
        strResults.push_back(results);
        strFeatures.push_back(features);
        strQsqrTime.push_back(qsqrTime);
        strMagicTime.push_back(magicTime);
        logFile << q <<" " << features << " " << qsqrTime << " " << magicTime << " " << decisionVector.back() << endl;
    }
    if (logFile.fail()) {
        LOG(ERRORL) << "Error writing to the log file";
    }
    logFile.close();
    string allResults = stringJoin(strResults);
    jsonResults->put("results", allResults);
    string allFeatures = stringJoin(strFeatures);
    jsonFeatures->put("features", allFeatures);
    string allQsqrTimes = stringJoin(strQsqrTime);
    jsonQsqrTime->put("qsqrtimes", allQsqrTimes);
    string allMagicTimes = stringJoin(strMagicTime);
    jsonMagicTime->put("magictimes", allMagicTimes);
}

void Training::execLiteralQuery(string& literalquery,
        EDBLayer& edb,
        Program& p,
        string& strResults,
        string& strFeatures,
        string& strQsqrTime,
        string& strMagicTime,
        uint64_t timeout,
        uint8_t repeatQuery,
        vector<Metrics>& featuresVector,
        vector<int>& decisionVector) {

    Dictionary dictVariables;
    Literal literal = p.parseLiteral(literalquery, dictVariables);
    Reasoner reasoner(1000000);

    Metrics metrics;
    reasoner.getMetrics(literal, NULL, NULL, edb, p, metrics, 5);
    featuresVector.push_back(metrics);
    stringstream strMetrics;
    strMetrics  << std::to_string(metrics.cost) << ","
        << std::to_string(metrics.estimate) << ","
        << std::to_string(metrics.countRules) << ","
        << std::to_string(metrics.countUniqueRules) << ","
        << std::to_string(metrics.countIntermediateQueries) << ","
        << std::to_string(metrics.countIDBPredicates);
    strFeatures += strMetrics.str();

    string algo="";
    uint8_t reps = 1;
    vector<double> qsqrTimes;
    vector<double> magicTimes;
    stringstream ssMagic;
    stringstream ssQsqr;
    while (reps <= repeatQuery) {
        algo = "magic";
        double durationMagic = Training::runAlgo(algo, reasoner, edb, p, literal, ssMagic, timeout);
        magicTimes.push_back(durationMagic);

        algo = "qsqr";
        double durationQsqr = Training::runAlgo(algo, reasoner, edb, p, literal, ssQsqr, timeout);
        qsqrTimes.push_back(durationQsqr);
        LOG(INFOL) << "Repetition " << reps << ") " << "Magic time = " << durationMagic << " , QSQR time = " << durationQsqr;
        reps++;
        if (durationMagic == timeout || durationQsqr == timeout) {
            break;
        }
    }

    double avgQsqrTime = accumulate(qsqrTimes.begin(), qsqrTimes.end(), 0.0)/qsqrTimes.size();
    double avgMagicTime = accumulate(magicTimes.begin(), magicTimes.end(), 0.0)/magicTimes.size();
    LOG(INFOL) << "Qsqr time : " << avgQsqrTime;
    LOG(INFOL) << "magic time: " << avgMagicTime;
    int winner = 1; // 1 is for QSQR
    if (avgMagicTime < avgQsqrTime) {
        winner = 0;
    }
    decisionVector.push_back(winner);
    strResults += ssQsqr.str();
    strQsqrTime += to_string(avgQsqrTime);
    strMagicTime += to_string(avgMagicTime);
}
