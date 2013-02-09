#include <cmath>
#include <ctime>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <stack>

#ifndef NOTHREADS
#include <thread>
#endif

#include "stochasticforest.hpp"
#include "datadefs.hpp"
#include "argparse.hpp"
#include "utils.hpp"
#include "math.hpp"
#include "options.hpp"

StochasticForest::StochasticForest() :
  forestType_(datadefs::forest_t::UNKNOWN) {
}

void StochasticForest::loadForest(const string& fileName) {

  ifstream forestStream(fileName.c_str());
  assert(forestStream.good());

  string newLine("");
  getline(forestStream, newLine);

  map<string,string> forestSetup = utils::parse(newLine, ',', '=', '"');

  forestType_ = datadefs::forestTypeAssign.at(forestSetup["FOREST"]);

  if ( forestType_ == forest_t::GBT ) {
    GBTShrinkage_ = utils::str2<num_t>(forestSetup["GBT_SHRINKAGE"]);
    vector<string> foo = utils::split(forestSetup["GBT_CONSTANTS"],',');
    GBTConstants_.resize(foo.size());
    for ( size_t i = 0; i < foo.size(); ++i ) {
      GBTConstants_[i] = utils::str2<num_t>(foo[i]);
    }
  }

  assert(forestSetup.find("NTREES") != forestSetup.end());
  size_t nTrees = static_cast<size_t>(utils::str2<int>(forestSetup["NTREES"]));
  rootNodes_.resize(nTrees);

  assert(forestSetup.find("TARGET") != forestSetup.end());
  targetName_ = forestSetup["TARGET"];

  assert(forestSetup.find("CATEGORIES") != forestSetup.end());
  categories_ = utils::split(forestSetup["CATEGORIES"], ',');
  isTargetNumerical_ = categories_.size() == 0;

  vector<string> foo = utils::split(forestSetup["QUANTILES"],',');
  quantiles_.resize(foo.size());
  transform(foo.begin(),foo.end(),quantiles_.begin(),utils::str2<num_t>);

  assert(forestStream.good());

  int treeIdx = -1;

  vector<map<string,Node*> > forestMap(nTrees);

  vector<size_t> nNodesPerTree(nTrees, 0);
  vector<size_t> nNodesAllocatedPerTree(nTrees, 0);

  // Read the forest
  while ( getline(forestStream, newLine) ) {

    // remove trailing end-of-line characters
    newLine = utils::chomp(newLine);

    // Empty lines will be discarded
    if (newLine == "") {
      continue;
    }

    if (newLine.compare(0, 5, "TREE=") == 0) {
      ++treeIdx;
      map<string,string> treeSetup = utils::parse(newLine, ',', '=', '"');
      assert( treeSetup.find("NNODES") != treeSetup.end() );
      nNodesPerTree[treeIdx] = utils::str2<size_t>(treeSetup["NNODES"]);
      continue;
    }
    
    //cout << newLine << endl;
    
    map<string,string> nodeMap = utils::parse(newLine, ',', '=', '"');

    // If the node is rootnode, we will create a new RootNode object and assign a reference to it in forestMap
    if (nodeMap["NODE"] == "*") {
      rootNodes_[treeIdx] = new RootNode();
      forestMap[treeIdx]["*"] = rootNodes_[treeIdx];
      rootNodes_[treeIdx]->reset(nNodesPerTree[treeIdx]);
    }

    Node* nodep = forestMap[treeIdx][nodeMap["NODE"]];
    
    string rawTrainPrediction = nodeMap["PRED"];
    num_t trainPrediction = datadefs::NUM_NAN;

    if ( this->isTargetNumerical() || (!this->isTargetNumerical() && forestType_ == forest_t::GBT) ) {
      trainPrediction = utils::str2<num_t>(rawTrainPrediction);
    }

    vector<string> foo2 = utils::split(nodeMap["DATA"],',');
    vector<num_t> trainData(foo2.size());
    transform(foo2.begin(),foo2.end(),trainData.begin(),utils::str2<num_t>);

    // Set train data prediction of the node
    nodep->setTrainPrediction(trainPrediction, rawTrainPrediction);
    nodep->setTrainData(trainData);
    
    // If the node has a splitter...
    if ( nodeMap.find("SPLITTER") != nodeMap.end() ) {

      size_t leftChildIdx = nNodesAllocatedPerTree[treeIdx]++;
      size_t rightChildIdx = nNodesAllocatedPerTree[treeIdx]++;
      
      if ( nNodesAllocatedPerTree[treeIdx] + 1 > nNodesPerTree[treeIdx] ) {
	cerr << "StochasticForest::loadForest() -- the tree contains more nodes than declared!" << endl;
	exit(1);
      }

      Node& lChild = rootNodes_[treeIdx]->childRef(leftChildIdx);
      Node& rChild = rootNodes_[treeIdx]->childRef(rightChildIdx);

      // If the splitter is numerical...
      if ( nodeMap["SPLITTERTYPE"] == "NUMERICAL" ) {

        nodep->setSplitter(nodeMap["SPLITTER"], utils::str2<num_t>(nodeMap["LVALUES"]), lChild, rChild);

      } else if ( nodeMap["SPLITTERTYPE"] == "CATEGORICAL" ){

        unordered_set<string> splitLeftValues = utils::keys(nodeMap["LVALUES"], ':');
        //set<string> splitRightValues = utils::keys(nodeMap["RVALUES"], ':');

        nodep->setSplitter(nodeMap["SPLITTER"], splitLeftValues, lChild, rChild);

      } else if ( nodeMap["SPLITTERTYPE"] == "TEXTUAL" ) {
	nodep->setSplitter(nodeMap["SPLITTER"], utils::str2<uint32_t>(nodeMap["LVALUES"]), lChild, rChild);
      } else {
	cerr << "ERROR: incompatible splitter type '" << nodeMap["SPLITTERTYPE"] << endl;
	exit(1);
      }

      assert( &lChild == nodep->leftChild() );
      assert( &rChild == nodep->rightChild() );

      forestMap[treeIdx][nodeMap["NODE"] + "L"] = nodep->leftChild();
      forestMap[treeIdx][nodeMap["NODE"] + "R"] = nodep->rightChild();

      // In case there is a branch for case when the splitter has missing value
      if ( nodeMap.find("M") != nodeMap.end() ) {
	size_t missingChildIdx = nNodesAllocatedPerTree[treeIdx]++;
	Node& mChild = rootNodes_[treeIdx]->childRef(missingChildIdx);
	nodep->setMissingChild(mChild);
	assert( &mChild == nodep->missingChild() );
	forestMap[treeIdx][nodeMap["NODE"] + "M"] = nodep->missingChild();
      }

    }

  }

  for ( size_t treeIdx = 0; treeIdx < nTrees; ++treeIdx ) {

    rootNodes_[treeIdx]->verifyIntegrity();

  }

  if ( forestType_ == forest_t::GBT ) {
    if ( ! isTargetNumerical_ ) {
      assert( GBTConstants_.size() == categories_.size() );
    }
    assert( GBTShrinkage_ > 0 );
  }

}

StochasticForest::~StochasticForest() {

  for (size_t treeIdx = 0; treeIdx < rootNodes_.size(); ++treeIdx) {
    delete rootNodes_[treeIdx];
  }

}

/* Prints the forest into a file, so that the forest can be loaded for later use (e.g. prediction).
 */
void StochasticForest::saveForest(ofstream& toFile) {

  if (forestType_ == forest_t::GBT) {
    toFile << "FOREST=GBT";
  } else if (forestType_ == forest_t::RF) {
    toFile << "FOREST=RF";
  } else if (forestType_ == forest_t::CART) {
    toFile << "FOREST=CART";
  } else {
    cerr << "StochasticForest::saveForest() -- Unknown forest type!" << endl;
    exit(1);
  }

  toFile << ",TARGET=" << "\"" << targetName_ << "\"";
  toFile << ",NTREES=" << rootNodes_.size();
  toFile << ",CATEGORIES=" << "\"";
  utils::write(toFile, categories_.begin(), categories_.end(), ',');
  toFile << "\"" << flush;
  if ( forestType_ == forest_t::GBT ) {
    toFile << ",GBT_CONSTANTS=\"" << flush;
    utils::write(toFile,GBTConstants_.begin(),GBTConstants_.end(),',');
    toFile << "\",GBT_SHRINKAGE=" << GBTShrinkage_<< flush;
  } 
  toFile << ",QUANTILES=\"" << flush;
  utils::write(toFile,quantiles_.begin(),quantiles_.end(),',');
  toFile << "\"" << endl;

  // Save each tree in the forest
  for (size_t treeIdx = 0; treeIdx < rootNodes_.size(); ++treeIdx) {
    toFile << "TREE=" << treeIdx << ",NNODES=" << rootNodes_[treeIdx]->nNodes() << ",NLEAVES=" << rootNodes_[treeIdx]->nLeaves() << endl;
    rootNodes_[treeIdx]->print(toFile);
  }

  // Close stream
  toFile.close();

}

bool StochasticForest::useQuantiles() const {
  return( quantiles_.size() > 0 );
}

void growTreesPerThread(vector<RootNode*>& rootNodes, Treedata* trainData,
    const size_t targetIdx, const ForestOptions* forestOptions,
    const distributions::PMF* pmf, distributions::Random* random) {

  for (size_t i = 0; i < rootNodes.size(); ++i) {
    rootNodes[i]->growTree(trainData, targetIdx, pmf, forestOptions, random);
  }

}

void StochasticForest::learnRF(Treedata* trainData, const size_t targetIdx,
    const ForestOptions* forestOptions, const vector<num_t>& featureWeights,
    vector<distributions::Random>& randoms) {

  forestType_ = forest_t::RF;
  targetName_ = trainData->feature(targetIdx)->name();
  isTargetNumerical_ = trainData->feature(targetIdx)->isNumerical();
  categories_ = trainData->feature(targetIdx)->categories();
  quantiles_ = forestOptions->quantiles;

  assert(trainData->nFeatures() == featureWeights.size());

  if ( fabs(featureWeights[targetIdx]) > datadefs::EPS) {
    cerr << "ERROR: Weight for the target variable must be 0!" << endl;
    exit(1);
  }

  rootNodes_.resize(forestOptions->nTrees);

  distributions::PMF pmf(featureWeights);

  if (forestOptions->isRandomSplit && forestOptions->mTry == 0) {
    cerr << "StochasticForest::learnRF() -- for randomized splits mTry must be greater than 0" << endl;
    exit(1);
  }

  size_t nThreads = randoms.size();

  assert(nThreads > 0);
  assert(forestOptions->nTrees > 0);
  assert(rootNodes_.size() == forestOptions->nTrees);

#ifdef NOTHREADS
  assert( nThreads == 1 );
#endif

  if (nThreads == 1) {

    vector<size_t> treeIcs = utils::range(forestOptions->nTrees);

    for (size_t treeIdx = 0; treeIdx < rootNodes_.size(); ++treeIdx) {
      rootNodes_[treeIdx] = new RootNode();
      rootNodes_[treeIdx]->growTree(trainData, targetIdx, &pmf, forestOptions, &randoms[0]);
    }

  }
#ifndef NOTHREADS  
  else {

    vector<vector<size_t> > treeIcs = utils::splitRange(forestOptions->nTrees, nThreads);

    vector <thread> threads;

    for ( size_t threadIdx = 0; threadIdx < nThreads; ++threadIdx ) {

      vector<size_t> &treeIcsPerThread = treeIcs[threadIdx];
      vector<RootNode*> rootNodesPerThread(treeIcsPerThread.size());

      for ( size_t i = 0; i < treeIcsPerThread.size(); ++i ) {

        rootNodes_[treeIcsPerThread[i]] = new RootNode();

        rootNodesPerThread[i] = rootNodes_[treeIcsPerThread[i]];

      }

      threads.push_back(thread(growTreesPerThread, 
			       rootNodesPerThread, 
			       trainData, 
			       targetIdx, 
			       forestOptions, 
			       &pmf, 
			       &randoms[threadIdx])); 
    }

    for ( size_t threadIdx = 0; threadIdx < threads.size(); ++threadIdx ) {
      threads[threadIdx].join();
    }
  }
#endif

  // Get features in the forest for fast look-up
  for ( size_t treeIdx = 0; treeIdx < rootNodes_.size(); ++treeIdx ) {
    set <size_t> featuresInTree = rootNodes_[treeIdx]->getFeaturesInTree();
    for ( set<size_t>::const_iterator it(featuresInTree.begin()); it != featuresInTree.end(); ++it ) {
      featuresInForest_.insert(*it);
    }
  }

}

void StochasticForest::learnGBT(Treedata* trainData, const size_t targetIdx,
    const ForestOptions* forestOptions, const vector<num_t>& featureWeights,
    vector<distributions::Random>& randoms) {

  forestType_ = forest_t::GBT;

  GBTShrinkage_ = forestOptions->shrinkage;
  
  targetName_ = trainData->feature(targetIdx)->name();

  isTargetNumerical_ = trainData->feature(targetIdx)->isNumerical();

  categories_ = trainData->feature(targetIdx)->categories();

  assert(trainData->nFeatures() == featureWeights.size());
  assert(fabs(featureWeights[targetIdx]) < datadefs::EPS);
  assert(randoms.size() == 1);

  distributions::PMF pmf(featureWeights);

  if (!isTargetNumerical_) {
    size_t nCategories = categories_.size();
    size_t nTrees = forestOptions->nTrees * nCategories;
    rootNodes_.resize(nTrees);
    GBTConstants_.resize(nCategories);
  } else {
    rootNodes_.resize(forestOptions->nTrees);
    GBTConstants_.resize(1);
  }

  //Allocates memory for the root nodes. With all these parameters, the RootNode is now able to take full control of the splitting process
  for (size_t treeIdx = 0; treeIdx < rootNodes_.size(); ++treeIdx) {
    rootNodes_[treeIdx] = new RootNode();
  }

  if (isTargetNumerical_) {
    this->growNumericalGBT(trainData, targetIdx, forestOptions, &pmf, randoms);
  } else {
    this->growCategoricalGBT(trainData, targetIdx, forestOptions, &pmf, randoms);
  }

  // Get features in the forest for fast look-up
  for ( size_t treeIdx = 0; treeIdx < rootNodes_.size(); ++treeIdx ) {
    set<size_t> featuresInTree = rootNodes_[treeIdx]->getFeaturesInTree();
    for ( set<size_t>::const_iterator it(featuresInTree.begin()); it != featuresInTree.end(); ++it ) {
      featuresInForest_.insert(*it);
    }
  }

}

// Grow a GBT "forest" for a numerical target variable
void StochasticForest::growNumericalGBT(Treedata* trainData,
					const size_t targetIdx, 
					const ForestOptions* forestOptions,
					const distributions::PMF* pmf, 
					vector<distributions::Random>& randoms) {

  assert(randoms.size() == 1);

  size_t nSamples = trainData->nSamples();
  // save a copy of the target column because it will be overwritten
  vector<num_t> trueTargetData = trainData->getFeatureData(targetIdx);

  // Target for GBT is different for each tree
  // reference to the target column, will overwrite it
  vector<num_t> curTargetData = trueTargetData;

  vector<size_t> sampleIcs = utils::range(nSamples);
  vector<size_t> missingIcs;
  trainData->separateMissingSamples(targetIdx,sampleIcs,missingIcs);
  assert(GBTConstants_.size() == 1);
  GBTConstants_[0] = math::mean(trainData->getFeatureData(targetIdx, sampleIcs));

  // Set the initial prediction to be the mean
  vector<num_t> prediction(nSamples, GBTConstants_[0]);

  for (size_t treeIdx = 0; treeIdx < rootNodes_.size(); ++treeIdx) {
    // current target is the negative gradient of the loss function
    // for 1/2*square loss, it is ( target - prediction )
    for (size_t i = 0; i < nSamples; i++) {
      curTargetData[i] = trueTargetData[i] - prediction[i];
    }

    trainData->replaceFeatureData(targetIdx, curTargetData);

    // Grow a tree to predict the current target
    rootNodes_[treeIdx]->growTree(trainData, targetIdx, pmf, forestOptions, &randoms[0]);

    // What kind of a prediction does the new tree produce?
    vector<num_t> curPrediction(nSamples); // = rootNodes_[treeIdx]->getTrainPrediction(); 
    for (size_t i = 0; i < nSamples; ++i) {
      curPrediction[i] = rootNodes_[treeIdx]->getTestPrediction(trainData, i);
    }

    // Calculate the current total prediction adding the newly generated tree
    for (size_t i = 0; i < nSamples; i++) {
      prediction[i] += GBTShrinkage_ * curPrediction[i];
    }

  }

  // GBT-forest is now done!
  // restore true target
  trainData->replaceFeatureData(targetIdx, trueTargetData);

}

// Grow a GBT "forest" for a categorical target variable
void StochasticForest::growCategoricalGBT(Treedata* trainData,
					  const size_t targetIdx, 
					  const ForestOptions* forestOptions,
					  const distributions::PMF* pmf, 
					  vector<distributions::Random>& randoms) {

  size_t nTrees = rootNodes_.size();

  size_t nCategories = categories_.size();

  // Each iteration consists of numClasses_ trees,
  // each of those predicting the probability residual for each class.
  size_t numIterations = nTrees / nCategories;

  // Save a copy of the target column because it will be overwritten later.
  // We also know that it must be categorical.
  size_t nSamples = trainData->nSamples();
  vector<num_t> trueTargetData = trainData->getFeatureData(targetIdx);
  vector < string > trueRawTargetData = trainData->getRawFeatureData(targetIdx);

  // Target for GBT is different for each tree.
  // We use the original target column to save each temporary target.
  // Reference to the target column, will overwrite it:
  vector<num_t> curTargetData = trueTargetData;

  for (size_t categoryIdx = 0; categoryIdx < nCategories; ++categoryIdx) {
    GBTConstants_[categoryIdx] = 0.0;
    for (size_t sampleIdx = 0; sampleIdx < nSamples; ++sampleIdx) {
      if (trueRawTargetData[sampleIdx] == categories_[categoryIdx]) {
        ++GBTConstants_[categoryIdx];
      }
    }
    GBTConstants_[categoryIdx] /= nSamples;
  }

  // Initialize class probability estimates and the predictions.
  // Note that dimensions in these two are reversed!
  vector<vector<num_t> > prediction(nSamples, GBTConstants_);
  vector<vector<num_t> > curPrediction(nCategories, vector<num_t>(nSamples, 0.0));
  vector<vector<num_t> > curProbability(nSamples, vector<num_t>(nCategories));

  for (size_t m = 0; m < numIterations; ++m) {
    // Multiclass logistic transform of class probabilities from current probability estimates.
    for (size_t i = 0; i < nSamples; ++i) {
      StochasticForest::transformLogistic(nCategories, prediction[i], curProbability[i]);
      // each prediction[i] is a vector<num_t>(numClasses_)
    }

    // construct a tree for each class
    for (size_t k = 0; k < nCategories; ++k) {
      // target for class k is ...
      for (size_t i = 0; i < nSamples; ++i) {
        // ... the difference between true target and current prediction
        curTargetData[i] = (categories_[k] == trueRawTargetData[i]) - curProbability[i][k];
      }

      // utils::write(cout,curTargetData.begin(),curTargetData.end());
      // cout << endl;

      // For each tree the target data becomes the recently computed residuals
      trainData->replaceFeatureData(targetIdx, curTargetData);

      // Grow a tree to predict the current target
      size_t treeIdx = m * nCategories + k; // tree index
      //cout << " " << treeIdx;
      rootNodes_[treeIdx]->growTree(trainData, targetIdx, pmf, forestOptions, &randoms[0]);

      //cout << "Tree " << treeIdx << " ready, predicting OOB samples..." << endl;

      // What kind of a prediction does the new tree produce
      // out of the whole training data set?
      curPrediction[k] = vector<num_t> (nSamples); //rootNodes_[treeIdx]->getTrainPrediction();
      for (size_t i = 0; i < nSamples; ++i) {
        curPrediction[k][i] = rootNodes_[treeIdx]->getTestPrediction(trainData, i);
      }

      // Calculate the current total prediction adding the newly generated tree
      for (size_t i = 0; i < nSamples; i++) {
        prediction[i][k] += GBTShrinkage_ * curPrediction[k][i];
      }
    }
  }

  // GBT-forest is now done!
  // restore the true target
  trainData->replaceFeatureData(targetIdx, trueRawTargetData);
}

void StochasticForest::transformLogistic(size_t nCategories,
    vector<num_t>& prediction, vector<num_t>& probability) {

  //size_t nCategories = trainData_->nCategories();

  // Multiclass logistic transform of class probabilities from current probability estimates.
  assert(nCategories == prediction.size());
  vector<num_t>& expPrediction = probability; // just using the space by a different name

  // find maximum prediction
  vector<num_t>::iterator maxPrediction = max_element(prediction.begin(),prediction.end());
  // scale by maximum to prevent numerical errors

  num_t expSum = 0.0;
  size_t k;
  for (k = 0; k < nCategories; ++k) {
    expPrediction[k] = exp(prediction[k] - *maxPrediction); // scale by maximum
    expSum += expPrediction[k];
  }
  for (k = 0; k < nCategories; ++k) {
    probability[k] = expPrediction[k] / expSum;
  }
}

num_t StochasticForest::error(const vector<num_t>& data1,
    const vector<num_t>& data2) {

  size_t nSamples = data1.size();
  assert(nSamples == data2.size());

  num_t predictionError = 0.0;
  size_t nRealSamples = 0;

  if (this->isTargetNumerical()) {
    for (size_t sampleIdx = 0; sampleIdx < nSamples; ++sampleIdx) {
      if (!datadefs::isNAN(data1[sampleIdx]) && !datadefs::isNAN(data2[sampleIdx])) {
        ++nRealSamples;
        predictionError += pow(data1[sampleIdx] - data2[sampleIdx], 2);
      }
    }
  } else {
    for (size_t sampleIdx = 0; sampleIdx < nSamples; ++sampleIdx) {
      if (!datadefs::isNAN(data1[sampleIdx]) && !datadefs::isNAN(data2[sampleIdx])) {
        ++nRealSamples;
        predictionError += 1.0 * (data1[sampleIdx] != data2[sampleIdx]);
      }
    }
  }

  if (nRealSamples > 0) {
    predictionError /= nRealSamples;
  } else {
    predictionError = datadefs::NUM_NAN;
  }

  return (predictionError);

}

/*
 num_t StochasticForest::getOobError() {

 vector<num_t> oobPredictions = this->getOobPredictions();

 size_t targetIdx = trainData_->getFeatureIdx(params_->targetStr);
 vector<num_t> trueData = trainData_->getFeatureData(targetIdx);

 return( this->error(oobPredictions,trueData) );

 }
 */

/*
 void StochasticForest::getImportanceValues(vector<num_t>& importanceValues, vector<num_t>& contrastImportanceValues) {

 this->getMeanMinimalDepthValues(importanceValues,contrastImportanceValues);
 return;

 if ( featuresInForest_.size() == 0 ) {
 cout << "NOTE: forest is empty!" << endl;
 }

 vector<num_t> oobPredictions = this->getOobPredictions();

 size_t nRealFeatures = trainData_->nFeatures();
 size_t nAllFeatures = 2*nRealFeatures;

 size_t targetIdx = trainData_->getFeatureIdx(params_->targetStr);

 vector<num_t> trueData = trainData_->getFeatureData(targetIdx);

 importanceValues.clear();
 importanceValues.resize(nAllFeatures,0.0);

 num_t oobError = this->error(oobPredictions,trueData);

 for ( set<size_t>::const_iterator it(featuresInForest_.begin()); it != featuresInForest_.end(); ++it ) {

 size_t featureIdx = *it;

 vector<num_t> permutedOobPredictions = this->getPermutedOobPredictions(featureIdx);

 num_t permutedOobError = this->error(permutedOobPredictions,trueData);

 importanceValues[featureIdx] = permutedOobError - oobError;

 if ( params_->normalizeImportanceValues ) {
 if ( oobError < datadefs::EPS ) {
 importanceValues[featureIdx] /= datadefs::EPS;
 } else {
 importanceValues[featureIdx] /= oobError;
 }
 }

 }

 assert( !datadefs::containsNAN(importanceValues) );


 contrastImportanceValues.resize(nRealFeatures);

 copy(importanceValues.begin() + nRealFeatures,
 importanceValues.end(),
 contrastImportanceValues.begin());

 importanceValues.resize(nRealFeatures);

 }
 */

void predictCatPerThread(Treedata* testData, 
			 vector<RootNode*>& rootNodes,
			 forest_t forestType,
			 vector<size_t>& sampleIcs, 
			 vector<string>* predictions,
			 vector<num_t>* confidence, 
			 vector<string>& categories,
			 vector<num_t>& GBTConstants, 
			 num_t& GBTShrinkage) {

  size_t nTrees = rootNodes.size();
  for (size_t i = 0; i < sampleIcs.size(); ++i) {

    size_t sampleIdx = sampleIcs[i];

    if (forestType == forest_t::GBT) {

      size_t nCategories = categories.size();
      num_t maxProb = 0;
      size_t maxProbCat = 0;

      for (size_t categoryIdx = 0; categoryIdx < nCategories; ++categoryIdx) {

        num_t cumProb = GBTConstants[categoryIdx];

        for (size_t iterIdx = 0; iterIdx < nTrees / nCategories; ++iterIdx) {
          size_t treeIdx = iterIdx * nCategories + categoryIdx;
          cumProb += GBTShrinkage * rootNodes[treeIdx]->getTestPrediction(testData, sampleIdx);
        }

        if (cumProb > maxProb) {
          maxProb = cumProb;
          maxProbCat = categoryIdx;
        }
      }

      (*predictions)[sampleIdx] = categories[maxProbCat];
      (*confidence)[sampleIdx] = 0.0;

      // MISSING: confidence
    } else {

      vector<string> predictionVec(nTrees);
      for ( size_t treeIdx = 0; treeIdx < nTrees; ++treeIdx ) {
        predictionVec[treeIdx] = rootNodes[treeIdx]->getRawTestPrediction(testData, sampleIdx);
      }

      (*predictions)[sampleIdx] = math::mode(predictionVec);
      (*confidence)[sampleIdx] = 1.0 * math::nMismatches(predictionVec, (*predictions)[sampleIdx]) / nTrees;
    }
  }
}

void predictNumPerThread(Treedata* testData, 
			 vector<RootNode*>& rootNodes,
			 forest_t forestType, 
			 vector<size_t>& sampleIcs,
			 vector<num_t>* predictions, 
			 vector<num_t>* confidence,
			 vector<num_t>& GBTConstants, 
			 num_t& GBTShrinkage) {

  size_t nTrees = rootNodes.size();
  for (size_t i = 0; i < sampleIcs.size(); ++i) {
    size_t sampleIdx = sampleIcs[i];
    vector<num_t> predictionVec(nTrees);
    for (size_t treeIdx = 0; treeIdx < nTrees; ++treeIdx) {
      predictionVec[treeIdx] = rootNodes[treeIdx]->getTestPrediction(testData,sampleIdx);
    }
    if (forestType == forest_t::GBT) {
      (*predictions)[sampleIdx] = GBTConstants[0];
      (*confidence)[sampleIdx] = 0.0;
      for (size_t treeIdx = 0; treeIdx < nTrees; ++treeIdx) {
	predictionVec[treeIdx] *= GBTShrinkage;
        (*predictions)[sampleIdx] += predictionVec[treeIdx];
      }
    } else {
      (*predictions)[sampleIdx] = math::mean(predictionVec);
    }
    (*confidence)[sampleIdx]  = sqrt(math::var(predictionVec));
  }
}

void StochasticForest::predict(Treedata* testData, vector<string>& predictions,vector<num_t>& confidence, size_t nThreads) {

  assert( nThreads > 0 );

  if ( forestType_ == forest_t::GBT && nThreads != 1 ) {
    cout << "NOTE: GBT does not support multithreading. Turning threads OFF... " << flush;
    nThreads = 1;
  }
  
  assert( ! this->isTargetNumerical() );

#ifdef NOTHREADS
  assert( nThreads == 1 );
#endif

  size_t nSamples = testData->nSamples();

  predictions.resize(nSamples);
  confidence.resize(nSamples);

  if (nThreads == 1) {

    vector<size_t> sampleIcs = utils::range(nSamples);

    predictCatPerThread(testData, rootNodes_, forestType_, sampleIcs, &predictions, &confidence, categories_, GBTConstants_, GBTShrinkage_);

  }
#ifndef NOTHREADS
  else {

    vector<vector<size_t> > sampleIcs = utils::splitRange(nSamples, nThreads);

    vector<thread> threads;

    for (size_t threadIdx = 0; threadIdx < nThreads; ++threadIdx) {
      // We only launch a thread if there are any samples allocated for prediction
      if (sampleIcs.size() > 0) {
        threads.push_back( thread(predictCatPerThread, testData, rootNodes_, forestType_, sampleIcs[threadIdx], &predictions, &confidence, categories_, GBTConstants_, GBTShrinkage_) );
      }
    }

    // Join all launched threads
    for (size_t threadIdx = 0; threadIdx < threads.size(); ++threadIdx) {
      threads[threadIdx].join();
    }
  }
#endif
}

void StochasticForest::predict(Treedata* testData, vector<num_t>& predictions,vector<num_t>& confidence, size_t nThreads) {

  assert( nThreads > 0 );

  if ( forestType_ == forest_t::GBT && nThreads != 1 ) {
    cout << "NOTE: GBT does not support multithreading. Turning threads OFF... " << flush;
    nThreads = 1;
  }

  assert( this->isTargetNumerical() );

#ifdef NOTHREADS
  assert( nThreads == 1 );
#endif

  size_t nSamples = testData->nSamples();

  predictions.resize(nSamples);
  confidence.resize(nSamples);

  if (nThreads == 1) {

    vector<size_t> sampleIcs = utils::range(nSamples);
    //cout << "1 thread!" << endl;
    predictNumPerThread(testData, rootNodes_, forestType_, sampleIcs, &predictions, &confidence, GBTConstants_, GBTShrinkage_);

  }
#ifndef NOTHREADS
  else {
    //cout << "More threads!" << endl;
    vector<vector<size_t> > sampleIcs = utils::splitRange(nSamples, nThreads);

    vector<thread> threads;

    for (size_t threadIdx = 0; threadIdx < nThreads; ++threadIdx) {
      // We only launch a thread if there are any samples allocated for prediction
      threads.push_back( thread(predictNumPerThread, testData, rootNodes_, forestType_, sampleIcs[threadIdx], &predictions, &confidence, GBTConstants_, GBTShrinkage_));
    }

    // Join all launched threads
    for (size_t threadIdx = 0; threadIdx < threads.size(); ++threadIdx) {
      threads[threadIdx].join();
    }
  }
#endif
}

void StochasticForest::predictQuantiles(Treedata* testData,
					vector<vector<num_t> >& predictions, 
					distributions::Random* random,
					const size_t nSamplesPerTree) {


  size_t nTrees = this->nTrees();
  size_t nSamples = testData->nSamples();
  size_t nQuantiles = quantiles_.size();

  assert(nQuantiles > 0);

  predictions.resize(nSamples,vector<num_t>(nQuantiles));
  
  for ( size_t sampleIdx = 0; sampleIdx < nSamples; ++sampleIdx ) {
    //cout << "sample " << sampleIdx << endl;
    vector<num_t> finalData(nTrees*nSamplesPerTree);
    for ( size_t treeIdx = 0; treeIdx < nTrees; ++treeIdx ) {
      vector<num_t> treeData = rootNodes_[treeIdx]->getChildLeafTrainData(testData,sampleIdx);
      //cout << "got leaf data for tree " << treeIdx << endl;
      for ( size_t i = 0; i < nSamplesPerTree; ++i ) {
	//cout << "sample " << i << " in tree" << endl;
	finalData[ treeIdx * nSamplesPerTree + i ] = treeData[ random->integer() % treeData.size() ];
      }
    }
    for ( size_t q = 0; q < nQuantiles; ++q ) {
      predictions[sampleIdx][q] = math::percentile(finalData,quantiles_[q]);
    }
    //cout << "done with sample " << sampleIdx << endl;
  }
  
}

/*
 vector<num_t> StochasticForest::getOobPredictions() {

 size_t nSamples = trainData_->nSamples();
 size_t nTrees = this->nTrees();
 vector<vector<num_t> > predictionMatrix(nSamples);
 vector<num_t> predictions(nSamples);
 for ( size_t treeIdx = 0; treeIdx < nTrees; ++treeIdx ) {
 vector<size_t> oobIcs = rootNodes_[treeIdx]->getOobIcs();
 for ( vector<size_t>::const_iterator it( oobIcs.begin() ); it != oobIcs.end(); ++it ) {
 size_t sampleIdx = *it;
 predictionMatrix[sampleIdx].push_back( rootNodes_[treeIdx]->getTrainPrediction(sampleIdx) );
 }
 }

 for ( size_t sampleIdx = 0; sampleIdx < nSamples; ++sampleIdx ) {
 predictions[sampleIdx] = this->isTargetNumerical() ? math::mean(predictionMatrix[sampleIdx]) : math::mode(predictionMatrix[sampleIdx]);
 }

 return( predictions );

 }
 */

/*
 vector<num_t> StochasticForest::getPermutedOobPredictions(const size_t featureIdx) {

 size_t nSamples = trainData_->nSamples();
 size_t nTrees = this->nTrees();
 vector<vector<num_t> > predictionMatrix(nSamples);
 vector<num_t> predictions(nSamples);
 for ( size_t treeIdx = 0; treeIdx < nTrees; ++treeIdx ) {
 vector<size_t> oobIcs = rootNodes_[treeIdx]->getOobIcs();
 for ( vector<size_t>::const_iterator it( oobIcs.begin() ); it != oobIcs.end(); ++it ) {
 size_t sampleIdx = *it;
 predictionMatrix[sampleIdx].push_back( rootNodes_[treeIdx]->getPermutedTrainPrediction(featureIdx,sampleIdx) );
 }
 }

 for ( size_t sampleIdx = 0; sampleIdx < nSamples; ++sampleIdx ) {
 predictions[sampleIdx] = this->isTargetNumerical() ? math::mean(predictionMatrix[sampleIdx]) : math::mode(predictionMatrix[sampleIdx]);
 }

 return( predictions );

 }
 */

/**
 Returns a vector of node counts in the trees of the forest
 */
size_t StochasticForest::nNodes() {

  size_t nNodes = 0;

  // Loop through all trees
  for (size_t treeIdx = 0; treeIdx < this->nTrees(); ++treeIdx) {

    // Get the node count for the tree
    nNodes += this->nNodes(treeIdx);

  }

  return (nNodes);
}

/**
 Returns the number of nodes in tree treeIdx
 */
size_t StochasticForest::nNodes(const size_t treeIdx) {
  return (rootNodes_[treeIdx]->nNodes());
}

/**
 Returns the number of trees in the forest
 */
size_t StochasticForest::nTrees() {
  return (rootNodes_.size());
}

void StochasticForest::getMeanMinimalDepthValues(Treedata* trainData,
    vector<num_t>& depthValues, vector<num_t>& contrastDepthValues) {

  if (featuresInForest_.size() == 0) {
    cout << "NOTE: featuresInForest_ is empty!" << endl;
  }

  size_t nRealFeatures = trainData->nFeatures();
  size_t nAllFeatures = 2 * nRealFeatures;

  depthValues.clear();
  depthValues.resize(nAllFeatures, 0.0);

  vector<size_t> featureCounts(nAllFeatures, 0);

  for (size_t treeIdx = 0; treeIdx < this->nTrees(); ++treeIdx) {

    vector<pair<size_t,size_t> > minDistPairs = rootNodes_[treeIdx]->getMinDistFeatures();

    for (size_t i = 0; i < minDistPairs.size(); ++i) {

      size_t featureIdx = minDistPairs[i].first;
      size_t newDepthValue = minDistPairs[i].second;

      ++featureCounts[featureIdx];

      depthValues[featureIdx] += 1.0 * (newDepthValue - depthValues[featureIdx]) / featureCounts[featureIdx];

    }

  }

  for (size_t featureIdx = 0; featureIdx < nAllFeatures; ++featureIdx) {
    if (featureCounts[featureIdx] == 0) {
      depthValues[featureIdx] = datadefs::NUM_NAN;
    }
  }

  contrastDepthValues.resize(nRealFeatures);

  copy(depthValues.begin() + nRealFeatures, depthValues.end(),
      contrastDepthValues.begin());

  depthValues.resize(nRealFeatures);

}
