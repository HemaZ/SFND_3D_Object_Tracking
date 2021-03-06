
#include <iostream>
#include <algorithm>
#include <numeric>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "camFusion.hpp"
#include "dataStructures.h"

using namespace std;


// Create groups of Lidar points whose projection into the camera falls into the same bounding box
void clusterLidarWithROI(std::vector<BoundingBox> &boundingBoxes, std::vector<LidarPoint> &lidarPoints, float shrinkFactor, cv::Mat &P_rect_xx, cv::Mat &R_rect_xx, cv::Mat &RT)
{
    // loop over all Lidar points and associate them to a 2D bounding box
    cv::Mat X(4, 1, cv::DataType<double>::type);
    cv::Mat Y(3, 1, cv::DataType<double>::type);

    for (auto it1 = lidarPoints.begin(); it1 != lidarPoints.end(); ++it1)
    {
        // assemble vector for matrix-vector-multiplication
        X.at<double>(0, 0) = it1->x;
        X.at<double>(1, 0) = it1->y;
        X.at<double>(2, 0) = it1->z;
        X.at<double>(3, 0) = 1;

        // project Lidar point into camera
        Y = P_rect_xx * R_rect_xx * RT * X;
        cv::Point pt;
        pt.x = Y.at<double>(0, 0) / Y.at<double>(0, 2); // pixel coordinates
        pt.y = Y.at<double>(1, 0) / Y.at<double>(0, 2);

        vector<vector<BoundingBox>::iterator> enclosingBoxes; // pointers to all bounding boxes which enclose the current Lidar point
        for (vector<BoundingBox>::iterator it2 = boundingBoxes.begin(); it2 != boundingBoxes.end(); ++it2)
        {
            // shrink current bounding box slightly to avoid having too many outlier points around the edges
            cv::Rect smallerBox;
            smallerBox.x = (*it2).roi.x + shrinkFactor * (*it2).roi.width / 2.0;
            smallerBox.y = (*it2).roi.y + shrinkFactor * (*it2).roi.height / 2.0;
            smallerBox.width = (*it2).roi.width * (1 - shrinkFactor);
            smallerBox.height = (*it2).roi.height * (1 - shrinkFactor);

            // check wether point is within current bounding box
            if (smallerBox.contains(pt))
            {
                enclosingBoxes.push_back(it2);
            }

        } // eof loop over all bounding boxes

        // check wether point has been enclosed by one or by multiple boxes
        if (enclosingBoxes.size() == 1)
        { 
            // add Lidar point to bounding box
            enclosingBoxes[0]->lidarPoints.push_back(*it1);
        }

    } // eof loop over all Lidar points
}


void show3DObjects(std::vector<BoundingBox> &boundingBoxes, cv::Size worldSize, cv::Size imageSize, bool bWait)
{
    // create topview image
    cv::Mat topviewImg(imageSize, CV_8UC3, cv::Scalar(255, 255, 255));

    for(auto it1=boundingBoxes.begin(); it1!=boundingBoxes.end(); ++it1)
    {
        // create randomized color for current 3D object
        cv::RNG rng(it1->boxID);
        cv::Scalar currColor = cv::Scalar(rng.uniform(0,150), rng.uniform(0, 150), rng.uniform(0, 150));

        // plot Lidar points into top view image
        int top=1e8, left=1e8, bottom=0.0, right=0.0; 
        float xwmin=1e8, ywmin=1e8, ywmax=-1e8;
        for (auto it2 = it1->lidarPoints.begin(); it2 != it1->lidarPoints.end(); ++it2)
        {
            // world coordinates
            float xw = (*it2).x; // world position in m with x facing forward from sensor
            float yw = (*it2).y; // world position in m with y facing left from sensor
            xwmin = xwmin<xw ? xwmin : xw;
            ywmin = ywmin<yw ? ywmin : yw;
            ywmax = ywmax>yw ? ywmax : yw;

            // top-view coordinates
            int y = (-xw * imageSize.height / worldSize.height) + imageSize.height;
            int x = (-yw * imageSize.width / worldSize.width) + imageSize.width / 2;

            // find enclosing rectangle
            top = top<y ? top : y;
            left = left<x ? left : x;
            bottom = bottom>y ? bottom : y;
            right = right>x ? right : x;

            // draw individual point
            cv::circle(topviewImg, cv::Point(x, y), 4, currColor, -1);
        }

        // draw enclosing rectangle
        cv::rectangle(topviewImg, cv::Point(left, top), cv::Point(right, bottom),cv::Scalar(0,0,0), 2);

        // augment object with some key data
        char str1[200], str2[200];
        sprintf(str1, "id=%d, #pts=%d", it1->boxID, (int)it1->lidarPoints.size());
        putText(topviewImg, str1, cv::Point2f(left-250, bottom+50), cv::FONT_ITALIC, 2, currColor);
        sprintf(str2, "xmin=%2.2f m, yw=%2.2f m", xwmin, ywmax-ywmin);
        putText(topviewImg, str2, cv::Point2f(left-250, bottom+125), cv::FONT_ITALIC, 2, currColor);  
    }

    // plot distance markers
    float lineSpacing = 2.0; // gap between distance markers
    int nMarkers = floor(worldSize.height / lineSpacing);
    for (size_t i = 0; i < nMarkers; ++i)
    {
        int y = (-(i * lineSpacing) * imageSize.height / worldSize.height) + imageSize.height;
        cv::line(topviewImg, cv::Point(0, y), cv::Point(imageSize.width, y), cv::Scalar(255, 0, 0));
    }

    // display image
    string windowName = "3D Objects";
    cv::namedWindow(windowName, 1);
    cv::imshow(windowName, topviewImg);

    if(bWait)
    {
        cv::waitKey(0); // wait for key to be pressed
    }
}


// associate a given bounding box with the keypoints it contains
void clusterKptMatchesWithROI(BoundingBox &boundingBox, std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, std::vector<cv::DMatch> &kptMatches)
{
    std::vector<cv::DMatch> newkptmatch;
    std::vector <double> distances;
    double meandistant;
    for (auto it = kptMatches.begin(); it != kptMatches.end(); ++it){
        cv::KeyPoint prvKeyPoint = kptsPrev[it->queryIdx]; 
        cv::KeyPoint curKeyPoint = kptsCurr[it->trainIdx]; 
        if(boundingBox.roi.contains(prvKeyPoint.pt) && boundingBox.roi.contains(curKeyPoint.pt)){
            cv::Point2f diff = curKeyPoint.pt - prvKeyPoint.pt;
            double distant = cv::sqrt(diff.x*diff.x + diff.y*diff.y);
            meandistant+= distant;
            distances.push_back(distant);
            newkptmatch.push_back(*it);
            continue;
        }
    }
    meandistant = meandistant/distances.size();
    for (size_t i = 0; i < distances.size(); i++)
    {
        if(distances[i]>meandistant){
            newkptmatch.erase(newkptmatch.begin()+i);
            distances.erase(distances.begin()+i);
        }
    }
    
    boundingBox.kptMatches = newkptmatch;
}


// Compute time-to-collision (TTC) based on keypoint correspondences in successive images
void computeTTCCamera(std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, 
                      std::vector<cv::DMatch> kptMatches, double frameRate, double &TTC, cv::Mat *visImg)
{
    // compute distance ratios between all matched keypoints
    vector<double> distRatios; // stores the distance ratios for all keypoints between curr. and prev. frame
    for (auto it1 = kptMatches.begin(); it1 != kptMatches.end() - 1; ++it1)
    { // outer kpt. loop

        // get current keypoint and its matched partner in the prev. frame
        cv::KeyPoint kpOuterCurr = kptsCurr.at(it1->trainIdx);
        cv::KeyPoint kpOuterPrev = kptsPrev.at(it1->queryIdx);

        for (auto it2 = kptMatches.begin() + 1; it2 != kptMatches.end(); ++it2)
        { // inner kpt.-loop

            double minDist = 100.0; // min. required distance

            // get next keypoint and its matched partner in the prev. frame
            cv::KeyPoint kpInnerCurr = kptsCurr.at(it2->trainIdx);
            cv::KeyPoint kpInnerPrev = kptsPrev.at(it2->queryIdx);

            // compute distances and distance ratios
            double distCurr = cv::norm(kpOuterCurr.pt - kpInnerCurr.pt);
            double distPrev = cv::norm(kpOuterPrev.pt - kpInnerPrev.pt);

            if (distPrev > std::numeric_limits<double>::epsilon() && distCurr >= minDist)
            { // avoid division by zero

                double distRatio = distCurr / distPrev;
                distRatios.push_back(distRatio);
            }
        } // eof inner loop over all matched kpts
    }     // eof outer loop over all matched kpts

    // only continue if list of distance ratios is not empty
    if (distRatios.size() == 0)
    {
        TTC = NAN;
        return;
    }

    // compute camera-based TTC from distance ratios
    double meanDistRatio = std::accumulate(distRatios.begin(), distRatios.end(), 0.0) / distRatios.size();
    // STUDENT TASK (replacement for meanDistRatio)
    std::sort(distRatios.begin(), distRatios.end());
    double medianRatio;
    size_t half = distRatios.size()/2;
    if(distRatios.size()%2==0){
        medianRatio = (distRatios.at(half) + distRatios.at(half-1))/2;
    }else{
        medianRatio = distRatios[half];
    }
    // Compute TTC
    double dT = 1 / frameRate;
    TTC = -dT / (1 - medianRatio);
}


void computeTTCLidar(std::vector<LidarPoint> &lidarPointsPrev,
                     std::vector<LidarPoint> &lidarPointsCurr, double frameRate, double &TTC, string ttc_method)
{
    // compute TTC based on median and average 
     // auxiliary variables
    double dT = 1/frameRate;        // time between two measurements in seconds
    double totalPrev = 0;
    double totalCurr = 0;
    vector<double> xPrev, XCur;
    // find closest distance to Lidar points within ego lane
    double minXPrev = 1e9, minXCurr = 1e9;
    for (auto it = lidarPointsPrev.begin(); it != lidarPointsPrev.end(); ++it)
    {
        minXPrev = minXPrev > it->x ? it->x : minXPrev;
        xPrev.push_back(it->x);
        totalPrev += it->x;
    }

    for (auto it = lidarPointsCurr.begin(); it != lidarPointsCurr.end(); ++it)
    {
        minXCurr = minXCurr > it->x ? it->x : minXCurr;
        XCur.push_back(it->x);
        totalCurr += it->x;
    }
    if(ttc_method.compare("MEDIAN") == 0){
        // Compute Median Prev
        std::sort(xPrev.begin(), xPrev.end());
        double medianPrev;
        size_t half = xPrev.size()/2;
        if(xPrev.size()%2==0){
            medianPrev = (xPrev.at(half) + xPrev.at(half-1))/2;
        }else{
            medianPrev = xPrev[half];
        }
        // Compute Median Curr
        std::sort(XCur.begin(), XCur.end());
        double medianCur;
        half = XCur.size()/2;
        if(XCur.size()%2==0){
            medianCur = (XCur.at(half) + XCur.at(half-1))/2;
        }else{
            medianCur = XCur[half];
        }
        // TTC Based on Median
        cout << "medianPrev " << medianPrev <<" ";
        cout << "MedianCurr " << medianCur << endl;
        TTC = minXCurr * dT / (medianPrev - medianCur);
        cout << "TTC Median Based " << TTC << endl;
    }else if(ttc_method.compare("AVERAGE") ==0){
        double avgPrev = totalPrev / lidarPointsPrev.size();
        double avgCur = totalCurr / lidarPointsCurr.size();
         // TTC Based on Average
        cout << "avgPrev " << avgPrev <<" ";
        cout << "avgCur " << avgCur << endl;
        TTC = minXCurr * dT / (avgPrev - avgCur);
        cout << "TTC Average Based " << TTC << endl;
    }else{
        // compute TTC from both measurements
        cout << "MinXPrev " << minXPrev <<" ";
        cout << "MinxCurr " << minXCurr << endl;
        TTC = minXCurr * dT / (minXPrev - minXCurr);
        cout << "TTC Minimum Point Based " << TTC << endl;
    }   
}


void matchBoundingBoxes(std::vector<cv::DMatch> &matches, std::map<int, int> &bbBestMatches, DataFrame &prevFrame, DataFrame &currFrame)
{
    vector < vector <int> > boxesTable (currFrame.boundingBoxes.size(), vector <int> (prevFrame.boundingBoxes.size(), 0)); // table shape is (curframeBoxes, PrevframeBoxes)
    // Lets first give unique ids for each bounding box
    int boxId = 0;
    for (auto it1 = prevFrame.boundingBoxes.begin(); it1 !=prevFrame.boundingBoxes.end(); ++it1)
        {
            it1->boxID = boxId++;
        }
    boxId = 0;
    for (auto it2 = currFrame.boundingBoxes.begin(); it2 !=currFrame.boundingBoxes.end(); ++it2)
        {
            it2->boxID = boxId++;
        }
    
    cv::Point2f prevP, curP;   
    for(auto it=matches.begin(); it!=matches.end(); ++it){
        prevP = prevFrame.keypoints[it->queryIdx].pt;
        curP = currFrame.keypoints[it->trainIdx].pt;
        std::vector<int> prevBBoxes;
        for (auto it2 = prevFrame.boundingBoxes.begin(); it2 !=prevFrame.boundingBoxes.end(); ++it2)
        {
            if(it2->roi.contains(prevP)){
                it2->kptMatches.push_back(*it);
                it2->keypoints.push_back(prevFrame.keypoints[it->queryIdx]);
                prevBBoxes.push_back(it2->boxID);
            }
        }
        for (auto it2 = currFrame.boundingBoxes.begin(); it2 !=currFrame.boundingBoxes.end(); ++it2)
        {
            if(it2->roi.contains(curP)){
                it2->kptMatches.push_back(*it);
                it2->keypoints.push_back(currFrame.keypoints[it->trainIdx]);
                for (auto previt = prevBBoxes.begin(); previt != prevBBoxes.end(); ++previt)
                {
                    boxesTable[it2->boxID][*previt] +=1;
                }
                
                
            }
        }
        
        
    }
    for (int row = 0; row < boxesTable.size(); row++) {
        int maxElIdx = max_element(boxesTable[row].begin(), boxesTable[row].end()) - boxesTable[row].begin();
        bbBestMatches.insert(pair<int, int>(row, maxElIdx));
    }

}
