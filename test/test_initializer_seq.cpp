#include <iostream>
#include <string>
#include <opencv2/opencv.hpp>

#include "feature_detector.hpp"
#include "initializer.hpp"
#include "config.hpp"
#ifdef WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

using std::string;

#ifdef WIN32
void loadImages(const string &file_directory, std::vector<string> &image_filenames)
{
    _finddata_t file;
    intptr_t fileHandle;
    std::string filename = file_directory + "\\*.jpg";
    fileHandle = _findfirst(filename.c_str(), &file);
    if (fileHandle  != -1L) {
        do {
            std::string image_name = file_directory + "\\" + file.name;
            image_filenames.push_back(image_name);
            std::cout << image_name << std::endl;
        } while (_findnext(fileHandle, &file) == 0);
    }

    _findclose(fileHandle);
    std::sort(image_filenames.begin(), image_filenames.end());
}
#else
void loadImages(const std::string &strFileDirectory, std::vector<string> &vstrImageFilenames)
{
    DIR* dir = opendir(strFileDirectory.c_str());
    dirent* p = NULL;
    while((p = readdir(dir)) != NULL)
    {
        if(p->d_name[0] != '.')
        {
            std::string imageFilename = strFileDirectory + string(p->d_name);
            vstrImageFilenames.push_back(imageFilename);
            //cout << imageFilename << endl;
        }
    }

    closedir(dir);
    std::sort(vstrImageFilenames.begin(),vstrImageFilenames.end());
}
#endif

std::string ssvo::Config::FileName;

int main(int argc, char const *argv[])
{
    if (argc != 2) {
        std::cout << "Usge: ./test_initializer path_to_sequence configflie" << std::endl;
    }

    ssvo::Config::FileName = std::string(argv[2]);

    std::string dir_name = argv[1];
    std::vector<string> img_file_names;
    loadImages(dir_name, img_file_names);

    cv::Mat K = ssvo::Config::cameraIntrinsic();
    cv::Mat DistCoef = ssvo::Config::cameraDistCoef();

    ssvo::Camera camera(ssvo::Config::imageWidth(), ssvo::Config::imageHeight(), K, DistCoef);
    ssvo::FastDetector fast_detector(500, 3);
    ssvo::Initializer initializer;

    int fps = ssvo::Config::cameraFps();
    cv::Mat cur_img;
    cv::Mat ref_img;
    int initial = 0;
    for(std::vector<std::string>::iterator i = img_file_names.begin(); i != img_file_names.end(); ++i)
    {
        cv::Mat img = cv::imread(*i, CV_LOAD_IMAGE_UNCHANGED);
        if(img.empty()) throw std::runtime_error("Could not open image: " + *i);

        cv::cvtColor(img, cur_img, cv::COLOR_RGB2GRAY);

        ssvo::Frame frame(cur_img, 0, std::make_shared<ssvo::Camera>(camera));

        if(initial == 0)
        {
            ref_img = cur_img.clone();
            frame.detectFeatures(fast_detector);
            std::cout << "All corners: " << frame.kps_.size() <<std::endl;
            int succeed = initializer.addFirstFrame(std::make_shared<ssvo::Frame>(frame));
            if(succeed == ssvo::SUCCESS) initial = 1;
        }
        else if(initial == 1)
        {
            ssvo::InitResult result = initializer.addSecondFrame(std::make_shared<ssvo::Frame>(frame));
            if(result == ssvo::RESET) {
                initial = 0;
                continue;
            }
            else if(result == ssvo::SUCCESS)
                break;

            std::vector<cv::Point2f> pts_ref, pts_cur;
            initializer.getTrackedPoints(pts_ref, pts_cur);


            cv::Mat match_img = img.clone();
            for(int i=0; i<pts_ref.size();i++)
            {
                cv::line(match_img, pts_cur[i], pts_ref[i],cv::Scalar(0,0,70));
            }

            cv::imshow("KeyPoints detectByImage", match_img);
        }

        cv::waitKey(fps);

    }

    cv::waitKey(0);

    return 0;
}