#include <future>
#include "config.hpp"
#include "utils.hpp"
#include "depth_filter.hpp"
#include "feature_alignment.hpp"
#include "image_alignment.hpp"

namespace ssvo{

void showMatch(const cv::Mat &src, const cv::Mat &dst, const Vector2d &epl0, const Vector2d &epl1, const Vector2d &px_src, const Vector2d &px_dst)
{
    cv::Mat src_show = src.clone();
    cv::Mat dst_show = dst.clone();
    if(src_show.channels() == 1)
        cv::cvtColor(src_show, src_show, CV_GRAY2RGB);
    if(dst_show.channels() == 1)
        cv::cvtColor(dst_show, dst_show, CV_GRAY2RGB);

    cv::Point2d p0(epl0[0], epl0[1]);
    cv::Point2d p1(epl1[0], epl1[1]);
    cv::Point2d p_src(px_src[0], px_src[1]);
    cv::Point2d p_dst(px_dst[0], px_dst[1]);

    cv::line(dst_show, p0, p1, cv::Scalar(0, 255, 0), 1);
    cv::circle(dst_show, p1, 3, cv::Scalar(0,255,0));
    cv::circle(src_show, p_src, 5, cv::Scalar(255,0,0));
    cv::circle(dst_show, p_dst, 5, cv::Scalar(255,0,0));

    cv::imshow("ref", src_show);
    cv::imshow("dst", dst_show);
    cv::waitKey(0);
}

void showEplMatch(const KeyFrame::Ptr &keyframe, const Frame::Ptr &frame, const SE3d &T_cur_from_ref,
                  int level_ref, int level_cur, const Vector3d &xyz_near, const Vector3d &xyz_far, const Vector3d &xyz_ref, const Vector2d &px_dst)
{
    cv::Mat src_show = keyframe->getImage(level_ref).clone();
    cv::Mat dst_show = frame->getImage(level_cur).clone();
    if(src_show.channels() == 1)
        cv::cvtColor(src_show, src_show, CV_GRAY2RGB);
    if(dst_show.channels() == 1)
        cv::cvtColor(dst_show, dst_show, CV_GRAY2RGB);

    Vector3d epl_near = xyz_near / xyz_near[2];
    Vector3d epl_far  = xyz_far / xyz_far[2];

    Vector2d epl0 = frame->cam_->project(epl_near);
    Vector2d epl1 = frame->cam_->project(epl_far);

    const double scale_cur = 1.0/(1<<level_cur);
    const double scale_ref = 1.0/(1<<level_ref);
    const Vector2d px_src = keyframe->cam_->project(xyz_ref);

    cv::Point2d pl0(epl0[0]*scale_cur, epl0[1]*scale_cur);
    cv::Point2d pl1(epl1[0]*scale_cur, epl1[1]*scale_cur);
    cv::Point2d p_src(px_src[0]*scale_ref, px_src[1]*scale_ref);
    cv::Point2d p_dst(px_dst[0], px_dst[1]);

    cv::line(dst_show, pl0, pl1, cv::Scalar(0, 255, 0), 1);
    cv::circle(dst_show, pl1, 2, cv::Scalar(0,0,255));
    cv::circle(src_show, p_src, 5, cv::Scalar(255,0,0));
    cv::circle(dst_show, p_dst, 5, cv::Scalar(255,0,0));

    //! ref
    SE3d T_ref_from_cur = T_cur_from_ref.inverse();
    epl_far = T_ref_from_cur.translation();
    epl_near = xyz_ref;
    epl0 = keyframe->cam_->project(epl_near);
    epl1 = keyframe->cam_->project(epl_far);
    pl0.x = epl0[0]*scale_ref;
    pl0.y = epl0[1]*scale_ref;
    pl1.x = epl1[0]*scale_ref;
    pl1.y = epl1[1]*scale_ref;

    cv::line(src_show, pl0, pl1, cv::Scalar(0, 255, 0), 1);
    cv::circle(src_show, pl1, 2, cv::Scalar(0,0,255));

    cv::imshow("ref", src_show);
    cv::imshow("dst", dst_show);
    cv::waitKey(0);
}

void showAffine(const cv::Mat &src, const Vector2d &px_ref, const Matrix2d &A_ref_cur, const int size, const int level)
{
    cv::Mat src_show = src.clone();
    if(src_show.channels() == 1)
        cv::cvtColor(src_show, src_show, CV_GRAY2RGB);

    const double half_size = size*0.5;
    const int factor = 1 << level;
    Vector2d tl = A_ref_cur * Vector2d(-half_size, -half_size) * factor;
    Vector2d tr = A_ref_cur * Vector2d(half_size, -half_size) * factor;
    Vector2d bl = A_ref_cur * Vector2d(-half_size, half_size) * factor;
    Vector2d br = A_ref_cur * Vector2d(half_size, half_size) * factor;
    cv::Point2d TL(tl[0]+px_ref[0], tl[1]+px_ref[1]);
    cv::Point2d TR(tr[0]+px_ref[0], tr[1]+px_ref[1]);
    cv::Point2d BL(bl[0]+px_ref[0], bl[1]+px_ref[1]);
    cv::Point2d BR(br[0]+px_ref[0], br[1]+px_ref[1]);
    cv::Scalar color(0, 255, 0);
    cv::line(src_show, TL, TR, color, 1);
    cv::line(src_show, TR, BR, color, 1);
    cv::line(src_show, BR, BL, color, 1);
    cv::line(src_show, BL, TL, color, 1);
    cv::imshow("AFFINE", src_show);
    cv::waitKey(0);
}

//! =================================================================================================
//! DepthFilter
DepthFilter::DepthFilter(const FastDetector::Ptr &fast_detector, const LocalMapper::Ptr &mapper, bool report, bool verbose) :
    fast_detector_(fast_detector), mapper_(mapper),
    report_(report), verbose_(report&&verbose),
    filter_thread_(nullptr), track_thread_enabled_(true)

{
    options_.max_kfs = 5;
    options_.max_seeds_buffer = 20;
    options_.max_features = Config::minCornersPerKeyFrame();
    options_.max_epl_length = 1000;
    options_.epl_dist2_threshold = 16 * Config::pixelUnSigma2();
    options_.klt_epslion = 0.0001;
    options_.align_epslion = 0.0001;
    options_.min_disparity = 100;
    options_.min_track_features = 50;

    options_.px_error_normlized = Config::pixelUnSigma();
}

void DepthFilter::enableTrackThread()
{
    track_thread_enabled_ = true;
}

void DepthFilter::disableTrackThread()
{
    track_thread_enabled_ = false;
}

void DepthFilter::startMainThread()
{
    if(filter_thread_ == nullptr)
        filter_thread_ = std::make_shared<std::thread>(std::bind(&DepthFilter::run, this));
}

void DepthFilter::stopMainThread()
{
    setStop();
    if(filter_thread_)
    {
        if(filter_thread_->joinable())
            filter_thread_->join();
        filter_thread_.reset();
    }
}

void DepthFilter::setStop()
{
    std::unique_lock<std::mutex> lock(mutex_stop_);
    stop_require_ = true;
}

bool DepthFilter::isRequiredStop()
{
    std::unique_lock<std::mutex> lock(mutex_stop_);
    return stop_require_;
}

void DepthFilter::run()
{
    while(!isRequiredStop())
    {
        Frame::Ptr frame_cur = checkNewFrame();
        if(frame_cur)
        {
            double t0 = (double)cv::getTickCount();
            int updated_count = updateSeeds(frame_cur);
            double t1 = (double)cv::getTickCount();
            int  project_count = reprojectAllSeeds(frame_cur);
            double t2 = (double)cv::getTickCount();
            LOG_IF(WARNING, report_) << "[DepthFilter][2] Frame: " << frame_cur->id_
                                     << ", Seeds after updated: " << updated_count
                                     << "(" << (t1-t0)/cv::getTickFrequency() << "ms)"
                                     << ", new reprojected: " << project_count
                                     << "(" << (t2-t1)/cv::getTickFrequency() << "ms)";
        }

    }
}

Frame::Ptr DepthFilter::checkNewFrame()
{
    static Frame::Ptr frame_last;
    std::unique_lock<std::mutex> lock(mutex_frame_);
    cond_process_main_.wait_for(lock, std::chrono::milliseconds(3));
    if(frames_buffer_.empty())
        return nullptr;

    Frame::Ptr frame = frames_buffer_.front();
    frames_buffer_.pop_front();
    return frame;
}

void DepthFilter::trackFrame(const Frame::Ptr &frame_last, const Frame::Ptr &frame_cur)
{
    if(track_thread_enabled_)
    {
        seeds_track_future_ = std::async(std::launch::async, &DepthFilter::trackSeeds, this, frame_last, frame_cur);
    }
    else
    {
        int tracked_count = trackSeeds(frame_last, frame_cur);
        LOG_IF(WARNING, report_) << "[DepthFilter][1] Frame: " << frame_cur->id_ << ", Tracking seeds: " << tracked_count;
    }
}

void DepthFilter::insertFrame(const Frame::Ptr &frame)
{
    LOG_ASSERT(frame != nullptr) << "[DepthFilter] Error input! Frame should not be null!";

    if(track_thread_enabled_)
    {
        seeds_track_future_.wait();
        int tracked_count = seeds_track_future_.get();
        LOG_IF(WARNING, report_) << "[DepthFilter][1] Frame: " << frame->id_ << ", Tracking seeds: " << tracked_count;
    }

    if(filter_thread_ == nullptr)
    {
        double t0 = (double)cv::getTickCount();
        int updated_count = updateSeeds(frame);
        double t1 = (double)cv::getTickCount();
        int  project_count = reprojectAllSeeds(frame);
        double t2 = (double)cv::getTickCount();
        LOG_IF(WARNING, report_) << "[DepthFilter][2] Frame: " << frame->id_
                                 << ", Seeds after updated: " << updated_count
                                 << "(" << (t1-t0)/cv::getTickFrequency() << "ms)"
                                 << ", new reprojected: " << project_count
                                 << "(" << (t2-t1)/cv::getTickFrequency() << "ms)";
    }
    else
    {
        std::unique_lock<std::mutex> lock(mutex_frame_);
        frames_buffer_.push_back(frame);
        cond_process_main_.notify_one();
    }
}

void DepthFilter::insertKeyFrame(const KeyFrame::Ptr &keyframe, const Frame::Ptr &frame)
{
    int new_seeds = createSeeds(keyframe, frame);
    mapper_->insertKeyFrame(keyframe);
    perprocessSeeds(keyframe);
    LOG(INFO) << "[DepthFilter] New created depth filter seeds: " << new_seeds;
}

//int DepthFilter::getSeedsForMapping(const KeyFrame::Ptr &keyframe, const Frame::Ptr &frame)
//{
//    LOG_ASSERT(keyframe) << "KeyFrame is null!";
//    LOG_ASSERT(frame) << "Frame is null!";
//
//    updateSeeds(frame);
//
//    std::vector<Feature::Ptr> seeds;
//    frame->getSeeds(seeds);
//    for(const Feature::Ptr & ft_seed : seeds)
//    {
//        const Seed::Ptr &seed = ft_seed->seed_;
//        keyframe->addSeed(ft_seed);
//        frame->removeSeed(seed);
//        earseSeed(seed->kf, seed);
//    }
//
//    return (int) seeds.size();
//}

int DepthFilter::createSeeds(const KeyFrame::Ptr &keyframe, const Frame::Ptr &frame)
{
    if(keyframe == nullptr)
        return 0;

    LOG_ASSERT(frame == nullptr || keyframe->frame_id_ == frame->id_) << "The keyframe " << keyframe->id_  << "(" << keyframe->frame_id_ << ") is not created from frame " << frame->id_;

    std::vector<Feature::Ptr> fts;
    keyframe->getFeatures(fts);

    std::vector<Feature::Ptr> seeds;
    keyframe->getSeeds(seeds);

    Corners old_corners;
    old_corners.reserve(fts.size()+seeds.size());
    for(const Feature::Ptr &ft : fts)
    {
        old_corners.emplace_back(Corner(ft->px_[0], ft->px_[1], 0, ft->level_));
    }
    for(const Feature::Ptr &ft : seeds)
    {
        old_corners.emplace_back(Corner(ft->px_[0], ft->px_[1], 0, ft->level_));
    }

    // TODO 如果对应的seed收敛，在跟踪过的关键帧增加观测？
    if(frame != nullptr)
    {
        std::vector<Feature::Ptr> seed_fts;
        frame->getSeeds(seed_fts);
        for(const Feature::Ptr &ft : seed_fts)
        {
            const Vector2d &px = ft->px_;
            old_corners.emplace_back(Corner(px[0], px[1], ft->level_, ft->level_));
        }
    }

    Corners new_corners;
    fast_detector_->detect(keyframe->images(), new_corners, old_corners, options_.max_features);

    if(new_corners.empty())
        return 0;

    double depth_mean;
    double depth_min;
    keyframe->getSceneDepth(depth_mean, depth_min);
    Seeds new_seeds;
    for(const Corner &corner : new_corners)
    {
        const Vector2d px(corner.x, corner.y);
        const Vector3d fn(keyframe->cam_->lift(px));
        new_seeds.emplace_back(Seed::create(keyframe, px, fn, corner.level, depth_mean, depth_min));
    }

    {
        std::unique_lock<std::mutex> lock(mutex_seeds_);
        seeds_buffer_.emplace_back(keyframe, std::make_shared<Seeds>(new_seeds));
        if(seeds_buffer_.size() > options_.max_seeds_buffer)
            seeds_buffer_.pop_front();
    }

    if(frame != nullptr)
    {
        for(const Seed::Ptr &seed : new_seeds)
        {
            Feature::Ptr new_ft = Feature::create(seed->px_ref, seed->level_ref, seed);
            frame->addSeed(new_ft);
        }
    }

//    std::string info;
//    for(const auto &it : seeds_buffer_)
//    {
//        info += "[" +  std::to_string(it.first->id_) + ", " + std::to_string(it.second->size()) + "], ";
//    }
//    LOG(ERROR) << info;

    return (int)new_seeds.size();
}

bool DepthFilter::perprocessSeeds(const KeyFrame::Ptr &keyframe)
{
    if(seeds_buffer_.empty() || seeds_buffer_.back().first != keyframe)
        return false;

    KeyFrame::Ptr reference_keyframe = keyframe->getRefKeyFrame();
    std::set<KeyFrame::Ptr> connect_keyframes = reference_keyframe->getConnectedKeyFrames(2);
    connect_keyframes.insert(reference_keyframe);

    Seeds &new_seeds = *seeds_buffer_.back().second;

    if(connect_keyframes.empty())
        return false;

    int project_count = 0;
    int matched_count = 0;
    for(const KeyFrame::Ptr &kf : connect_keyframes)
    {
        Vector2d px_matched;
        int level_matched;
        int matched_count_cur = reprojectSeeds(keyframe, new_seeds, kf);

        matched_count+=matched_count_cur;
        if(matched_count_cur == 0)
            break;
    }
    LOG(ERROR) << "[DepthFilter] [*] perprocess, matched " << matched_count;

    return true;
}

int DepthFilter::trackSeeds(const Frame::Ptr &frame_last, const Frame::Ptr &frame_cur) const
{
    if(frame_cur == nullptr || frame_last == nullptr)
        return 0;

    //! track seeds by klt
    std::vector<Feature::Ptr> seed_fts;
    frame_last->getSeeds(seed_fts);
    const int N = seed_fts.size();
    std::vector<cv::Point2f> pts_to_track;
    pts_to_track.reserve(N);
    for(int i = 0; i < N; i++)
    {
        pts_to_track.emplace_back(cv::Point2f((float)seed_fts[i]->px_[0], (float)seed_fts[i]->px_[1]));
    }

    if(pts_to_track.empty())
        return 0;

    std::vector<cv::Point2f> pts_tracked = pts_to_track;
    std::vector<bool> status;
    static cv::TermCriteria termcrit(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 30, options_.klt_epslion);
    utils::kltTrack(frame_last->opticalImages(), frame_cur->opticalImages(), Frame::optical_win_size_,
                    pts_to_track, pts_tracked, status, termcrit, true, verbose_);

    //! erase untracked seeds
    int tracked_count = 0;
    for(int i = 0; i < N; i++)
    {
        if(status[i])
        {
            const cv::Point2f &px = pts_tracked[i];
            Feature::Ptr new_ft = Feature::create(Vector2d(px.x, px.y), seed_fts[i]->level_, seed_fts[i]->seed_);
            frame_cur->addSeed(new_ft);
            tracked_count++;
        }
    }

    return tracked_count;
}

int DepthFilter::updateSeeds(const Frame::Ptr &frame)
{
    //! remove error tracked seeds and update
    std::vector<Feature::Ptr> seed_fts;
    frame->getSeeds(seed_fts);
    std::map<KeyFrame::Ptr, std::deque<Feature::Ptr> > seeds_map;
    for(const Feature::Ptr &ft : seed_fts)
    {
        const auto seeds_map_itr = seeds_map.lower_bound(ft->seed_->kf);
        if(seeds_map_itr == seeds_map.end())
        {
            std::deque<Feature::Ptr> new_deque;
            new_deque.push_back(ft);
            seeds_map.emplace(ft->seed_->kf, new_deque);
        }
        else
        {
            seeds_map_itr->second.push_back(ft);
        }
    }

//    static double px_error_angle = atan(0.5*Config::pixelUnSigma())*2.0;
    int updated_count = 0;
    for(const auto &seed_map : seeds_map)
    {
        KeyFrame::Ptr kf = seed_map.first;
        const std::deque<Feature::Ptr> &seeds_deque = seed_map.second;
        const SE3d T_cur_from_ref = frame->Tcw() * kf->pose();
        const SE3d T_ref_from_cur = T_cur_from_ref.inverse();
        for(const Feature::Ptr &ft : seeds_deque)
        {
            const Vector3d fn_cur = frame->cam_->lift(ft->px_);
            const Seed::Ptr &seed = ft->seed_;
            double err2 = utils::Fundamental::computeErrorSquared(
                kf->pose().translation(), seed->fn_ref/seed->getInvDepth(), T_cur_from_ref, fn_cur.head<2>());

            if(err2 > options_.epl_dist2_threshold)
            {
                frame->removeSeed(seed);
                continue;
            }

            //! update
            double depth = -1;
            if(utils::triangulate(T_cur_from_ref.rotationMatrix(), T_cur_from_ref.translation(), seed->fn_ref, fn_cur, depth))
            {
//                double tau = seed->computeTau(T_ref_from_cur, seed->fn_ref, depth, px_error_angle);
                double tau = seed->computeVar(T_cur_from_ref, depth, options_.klt_epslion*options_.px_error_normlized);
//                tau = tau + Config::pixelUnSigma();
                seed->update(1.0/depth, tau*tau);

                //! check converge
                if(seed->checkConvergence())
                {
                    createFeatureFromSeed(seed);
                    earseSeed(seed->kf, seed);
                    frame->removeSeed(seed);
                    continue;
                }
            }

            updated_count++;
        }
    }

    return updated_count;
}

int DepthFilter::reprojectSeeds(const KeyFrame::Ptr& keyframe, Seeds &seeds, const Frame::Ptr &frame)
{
    SE3d T_cur_from_ref = frame->Tcw() * keyframe->pose();
    SE3d T_ref_from_cur = T_cur_from_ref.inverse();

    int project_count = 0;
    int matched_count = 0;
    Vector2d px_matched;
    int level_matched;
    auto seed_itr = seeds.begin();
    for(; seed_itr != seeds.end();)
    {
        const Seed::Ptr &seed = *seed_itr;
        if(frame->seed_tracked_.count(seed))
        {
            seed_itr++;
            continue;
        }

        project_count++;
        bool matched = findEpipolarMatch(seed, keyframe, frame, T_cur_from_ref, px_matched, level_matched);
        if(!matched)
        {
            seed_itr++;
            continue;
        }

        //! check distance to epl, incase of the aligen draft
        Vector2d fn_matched = frame->cam_->lift(px_matched).head<2>();
        double dist2 = utils::Fundamental::computeErrorSquared(keyframe->pose().translation(), seed->fn_ref/seed->getInvDepth(), T_cur_from_ref, fn_matched);
        if(dist2 > options_.epl_dist2_threshold)
        {
            seed_itr++;
            continue;
        }

        double depth = -1;
        const Vector3d fn_cur = frame->cam_->lift(px_matched);
        bool succeed = utils::triangulate(T_cur_from_ref.rotationMatrix(), T_cur_from_ref.translation(), seed->fn_ref, fn_cur, depth);
        if(!succeed)
        {
            seed_itr++;
            continue;
        }

//            double tau = seed->computeTau(T_ref_from_cur, seed->fn_ref, depth, px_error_angle);
        double tau = seed->computeVar(T_cur_from_ref, depth, options_.align_epslion*options_.px_error_normlized);
//            tau = tau + Config::pixelUnSigma();
        seed->update(1.0/depth, tau*tau);

        //! check converge
        if(seed->checkConvergence())
        {
            createFeatureFromSeed(seed);
            seed_itr = seeds.erase(seed_itr);
            continue;
        }

        //! update px
        Feature::Ptr new_ft = Feature::create(px_matched, level_matched, seed);
        frame->addSeed(new_ft);
        matched_count++;
        seed_itr++;
    }

    return matched_count;
}

int DepthFilter::reprojectAllSeeds(const Frame::Ptr &frame)
{
    //! get new seeds for track
    std::set<KeyFrame::Ptr> candidate_keyframes = frame->getRefKeyFrame()->getConnectedKeyFrames(options_.max_kfs);
    candidate_keyframes.insert(frame->getRefKeyFrame());

    std::vector<Feature::Ptr> seed_fts;
    frame->getSeeds(seed_fts);
    frame->seed_tracked_.clear();
    for(const Feature::Ptr &ft : seed_fts)
        frame->seed_tracked_.insert(ft->seed_);

    int matched_count = 0;
    auto buffer_itr = seeds_buffer_.begin();
    for(;buffer_itr != seeds_buffer_.end();)
    {
        KeyFrame::Ptr keyframe = buffer_itr->first;
        if(!candidate_keyframes.count(keyframe) || keyframe->frame_id_ == frame->id_)
        {
            buffer_itr++;
            continue;
        }

        Seeds &seeds = *buffer_itr->second;

        matched_count += reprojectSeeds(keyframe, seeds, frame);

        if(seeds.empty())
            buffer_itr = seeds_buffer_.erase(buffer_itr);
        else
            buffer_itr++;
    }

    return matched_count;
}

void DepthFilter::createFeatureFromSeed(const Seed::Ptr &seed)
{
    //! create new feature
    // TODO add_observation 不用，需不需要找一下其他关键帧是否观测改点，增加约束
    // TODO 把这部分放入一个队列，在单独线程进行处理
    MapPoint::Ptr mpt = MapPoint::create(seed->kf->Twc() * (seed->fn_ref/seed->getInvDepth()));
    Feature::Ptr ft = Feature::create(seed->px_ref, seed->fn_ref, seed->level_ref, mpt);
    seed->kf->addFeature(ft);
    mapper_->map_->insertMapPoint(mpt);
    mpt->addObservation(seed->kf, ft);
    mpt->updateViewAndDepth();
    mapper_->addOptimalizeMapPoint(mpt);
//    std::cout << " Create new seed as mpt: " << ft->mpt_->id_ << ", " << 1.0/seed->getInvDepth() << ", kf: " << seed->kf->id_ << " his: ";
//    for(const auto his : seed->history){ std::cout << "[" << his.first << "," << his.second << "]";}
//    std::cout << std::endl;
}

bool DepthFilter::earseSeed(const KeyFrame::Ptr &keyframe, const Seed::Ptr &seed)
{
    //! earse seed
    auto buffer_itr = seeds_buffer_.begin();
    for(; buffer_itr != seeds_buffer_.end(); buffer_itr++)
    {
        if(keyframe != buffer_itr->first)
            continue;

        Seeds &seeds = *buffer_itr->second;
        auto seeds_itr = seeds.begin();
        for(; seeds_itr != seeds.end(); seeds_itr++)
        {
            if(seed != *seeds_itr)
                continue;

            seeds_itr = seeds.erase(seeds_itr);
            if(seeds.empty())
                seeds_buffer_.erase(buffer_itr);

            return true;
        }
    }

    return false;
}

bool DepthFilter::findEpipolarMatch(const Seed::Ptr &seed,
                                    const KeyFrame::Ptr &keyframe,
                                    const Frame::Ptr &frame,
                                    const SE3d &T_cur_from_ref,
                                    Vector2d &px_matched,
                                    int &level_matched)
{
    static const int patch_size = AlignPatch::Size;
//    static const int patch_area = AlignPatch::Area;
    static const int half_patch_size = AlignPatch::HalfSize;

    //! check if in the view of current frame
    const double z_ref = 1.0/seed->getInvDepth();
    const Vector3d xyz_ref(seed->fn_ref * z_ref);
    const Vector3d xyz_cur(T_cur_from_ref * xyz_ref);
    const double z_cur = xyz_cur[2];
    if(z_cur < 0.001f)
        return false;

    //! d - inverse depth, z - depth
    const double sigma = std::sqrt(seed->getVariance());
    const double d_max = z_ref + sigma;
    const double d_min = MAX(z_ref- sigma, 0.00000001f);
    const double z_ref_min = 1.0/d_max;
    const double z_ref_max = 1.0/d_min;

    //! calculate best search level
    const int level_ref = seed->level_ref;
    const int level_cur = MapPoint::predictScale(z_ref, z_cur, level_ref, frame->max_level_);
    level_matched = level_cur;
    const double scale_cur = 1.0 / (1 << level_cur);

    const Vector2d px_cur(frame->cam_->project(xyz_cur) * scale_cur);
    if(!frame->cam_->isInFrame(px_cur.cast<int>(), half_patch_size, level_cur))
        return false;

    //! px in image plane
    Vector3d xyz_near = T_cur_from_ref * (seed->fn_ref * z_ref_min);
    Vector3d xyz_far  = T_cur_from_ref * (seed->fn_ref * z_ref_max);

    //! Pc = R*Pr + t = z * R*Pn + t
    if(xyz_near[2] < 0.001f)
    {
        const Vector3d t = T_cur_from_ref.translation();
        const Vector3d R_fn = T_cur_from_ref.rotationMatrix() * seed->fn_ref;
        double z_ref_min_adjust = (0.001f - t[2]) / R_fn[2];
        xyz_near = z_ref_min_adjust * R_fn + t;
    }

    Vector2d px_near = frame->cam_->project(xyz_near) * scale_cur;
    Vector2d px_far  = frame->cam_->project(xyz_far) * scale_cur;
    Vector2d epl_px_dir(px_near - px_far);
    epl_px_dir.normalize();

    //! make search pixel all within image
    const int min_sample_width = half_patch_size;
    const int min_sample_height = half_patch_size;
    const int max_sample_width = frame->cam_->width() * scale_cur;
    const int max_sample_height = frame->cam_->height() * scale_cur;
    if(px_near[0] <= min_sample_width)
    {
        double adjust = ceil(min_sample_width - px_near[0])/epl_px_dir[0];
        px_near.noalias() += adjust * epl_px_dir;
    }
    else if(px_near[0] >= max_sample_width)
    {
        double adjust = floor(max_sample_width - px_near[0])/epl_px_dir[0];
        px_near.noalias() += adjust * epl_px_dir;
    }

    if(px_near[1] <= min_sample_height)
    {
        double adjust = ceil(min_sample_height - px_near[1])/epl_px_dir[1];
        px_near.noalias() += adjust * epl_px_dir;
    }
    else if(px_near[1] >= max_sample_height)
    {
        double adjust = floor(max_sample_height - px_near[1])/epl_px_dir[1];
        px_near.noalias() += adjust * epl_px_dir;
    }

    if(px_near[0] <= min_sample_width || px_near[0] >= max_sample_width ||
        px_near[1] <= min_sample_height || px_near[1] >= max_sample_height)
        return false;

    if(px_far[0] <= min_sample_width)
    {
        double adjust = ceil(min_sample_width - px_far[0])/epl_px_dir[0];
        px_far.noalias() += adjust * epl_px_dir;
    }
    else if(px_far[0] >= max_sample_width)
    {
        double adjust = floor(max_sample_width - px_far[0])/epl_px_dir[0];
        px_far.noalias() += adjust * epl_px_dir;
    }

    if(px_far[1] <= min_sample_height)
    {
        double adjust = ceil(min_sample_height - px_far[1])/epl_px_dir[1];
        px_far.noalias() += adjust * epl_px_dir;
    }
    else if(px_far[1] >= max_sample_height)
    {
        double adjust = floor(max_sample_height - px_far[1])/epl_px_dir[1];
        px_far.noalias() += adjust * epl_px_dir;
    }

    if(px_far[0] <= min_sample_width || px_far[0] >= max_sample_width ||
        px_far[1] <= min_sample_height || px_far[1] >= max_sample_height)
        return false;

    //! reject the seed whose epl is too long
    //! it maybe not nessary
    double epl_length = (px_near-px_far).norm();
    if(epl_length > options_.max_epl_length)
        return false;

    //! get px in normilzed plane
    xyz_near = frame->cam_->lift(px_near / scale_cur);
    xyz_far = frame->cam_->lift(px_far / scale_cur);
    Vector2d epl_dir = (xyz_near - xyz_far).head<2>();

    //! get warp patch
    Matrix2d A_cur_from_ref;

    utils::getWarpMatrixAffine(keyframe->cam_, frame->cam_, seed->px_ref, seed->fn_ref, level_ref,
                               z_ref, T_cur_from_ref, patch_size, A_cur_from_ref);

//    double det = A_cur_from_ref.determinant() / factor;
//    std::cout << "***** det: " <<  det << std::endl;

    static const int patch_border_size = patch_size+2;
    cv::Mat image_ref = keyframe->getImage(level_ref);
    Matrix<float, patch_border_size, patch_border_size, RowMajor> patch_with_border;
    utils::warpAffine<float, patch_border_size>(image_ref, patch_with_border, A_cur_from_ref,
                                                seed->px_ref, level_ref, level_cur);

    Matrix<float, patch_size, patch_size, RowMajor> patch;
    patch = patch_with_border.block(1, 1, patch_size, patch_size);

    const cv::Mat image_cur = frame->getImage(level_cur);

    Vector2d px_best(-1,-1);
    Vector3d estimate(0,0,0);
    if(epl_length > 2.0)
    {
        int n_steps = epl_length / 0.707;
        Vector2d step = epl_dir / n_steps;

        // TODO 使用模板来加速！！！
        //! SSD
        double t0 = (double)cv::getTickCount();
        ZSSD<float, patch_size> zssd(patch);
        double score_best = std::numeric_limits<double>::max();
        double score_second = score_best;
        int index_best = -1;
        int index_second = -1;

        Vector2d fn_start = xyz_far.head<2>() - step * 2;
        n_steps += 2;
        Vector2d fn(fn_start);
        for(int i = 0; i < n_steps; ++i, fn += step)
        {
            Vector2f px = (frame->cam_->project(fn[0], fn[1]) * scale_cur).cast<float>();

            //! always in frame's view
            if(!frame->cam_->isInFrame(px.cast<int>(), half_patch_size, level_cur))
                continue;

            Matrix<float, patch_size, patch_size, RowMajor> patch_cur;
            utils::interpolateMat<uchar, float, patch_size>(image_cur, patch_cur, px[0], px[1]);

            float score = zssd.compute_score(patch_cur);

            if(score < score_best)
            {
                score_best = score;
                index_best = i;
            }
            else if(score < score_second)
            {
                score_second = score;
                index_second = i;
            }
        }

        if(score_best > 0.8 * score_second && std::abs(index_best - index_second) > 2)
            return false;

        if(score_best > zssd.threshold())
            return false;

        Vector2d pn_best = fn_start + index_best * step;
        px_best = frame->cam_->project(pn_best[0], pn_best[1]) * scale_cur;

        double t1 = (double)cv::getTickCount();
        double time = (t1-t0)/cv::getTickFrequency();
        LOG_IF(INFO, verbose_) << " Step: " << n_steps << " T:" << time << "(" << time/n_steps << ")"
                               << " best: [" << index_best << ", " << score_best<< "]"
                               << " second: [" << index_second << ", " << score_second<< "]";
    }
    else
    {
        px_best = (px_near + px_far) * 0.5;
    }

    estimate.head<2>() = px_best;
    if(!AlignPatch::align2DI(image_cur, patch_with_border, estimate, 30, options_.align_epslion, verbose_))
    {
//        std::cout << "dx:\n " << dx << std::endl;
//        std::cout << "dy:\n " << dy << std::endl;

        static bool show = false;
        if(show)
        {
            //        showMatch(keyframe->getImage(level_ref), current_frame_->getImage(level_cur), px_near, px_far, ft->px/factor, px_best);
//            DISPLAY:
            showEplMatch(keyframe, frame, T_cur_from_ref, level_ref, level_cur, xyz_near, xyz_far, xyz_ref, px_best);
            showAffine(keyframe->getImage(level_ref), seed->px_ref * scale_cur, A_cur_from_ref.inverse(), 8, level_ref);
        }

        return false;
    }

    //! transform to level-0
    px_matched = estimate.head<2>() / scale_cur;

    LOG_IF(INFO, verbose_) << "Found! [" << seed->px_ref.transpose() << "] "
                           << "dst: [" << px_matched.transpose() << "] "
                           << "epl: [" << px_near.transpose() << "]--[" << px_far.transpose() << "]" << std::endl;

//    showMatch(image_ref, image_cur, px_near, px_far, seed.ft->px/factor, px_matched/factor);

    return true;
}

}