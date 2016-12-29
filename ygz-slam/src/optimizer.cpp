// g2o
#include <g2o/core/sparse_optimizer.h>
#include <g2o/core/block_solver.h>
#include <g2o/core/solver.h>
#include <g2o/core/robust_kernel_impl.h>
#include <g2o/core/optimization_algorithm_levenberg.h>
#include <g2o/solvers/csparse/linear_solver_csparse.h>
#include <g2o/solvers/eigen/linear_solver_eigen.h>
#include <g2o/types/sba/types_six_dof_expmap.h>

// boost
#include <boost/math/distributions/normal.hpp>

#include <opencv2/imgproc/imgproc.hpp>

#include "ygz/common_include.h"
#include "ygz/optimizer.h"
#include "ygz/g2o_types.h"
#include "ygz/ceres_types.h"
#include "ygz/memory.h"
#include "ygz/local_mapping.h"

using namespace g2o;

namespace ygz
{

namespace opti
{

void TwoViewBAG2O (
    const long unsigned int& frameID1,
    const long unsigned int& frameID2
)
{
    assert ( Memory::GetFrame ( frameID1 ) != nullptr && Memory::GetFrame ( frameID2 ) != nullptr );

    // set up g2o
    typedef g2o::BlockSolver_6_3 Block;
    Block::LinearSolverType* linearSolver = new LinearSolverEigen<Block::PoseMatrixType>();
    Block* solver_ptr = new Block ( linearSolver );
    OptimizationAlgorithmLevenberg* solver = new OptimizationAlgorithmLevenberg ( solver_ptr );
    g2o::SparseOptimizer optimizer;
    optimizer.setAlgorithm ( solver );

    // vertecies and edges
    VertexSE3Sophus* v1 = new VertexSE3Sophus();
    v1->setId ( 0 );
    Frame* frame1 = Memory::GetFrame ( frameID1 );
    v1->setEstimate ( frame1->_T_c_w.log() );

    VertexSE3Sophus* v2 = new VertexSE3Sophus();
    v2->setId ( 1 );
    Frame* frame2 = Memory::GetFrame ( frameID2 );
    v2->setEstimate ( frame2->_T_c_w.log() );

    optimizer.addVertex ( v1 );
    optimizer.addVertex ( v2 );
    optimizer.setVerbose ( false );

    v1->setFixed ( true ); // fix the first one

    // points and edges
    map<unsigned long, VertexSBAPointXYZ*> vertex_points;
    vector<EdgeSophusSE3ProjectXYZ*> edges;
    int pts_id = 2;

    /** * debug only
    // print all related data
    for ( auto iter = frame1->_map_point.begin(); iter!=frame1->_map_point.end(); iter++ ) {
        MapPoint::Ptr map_point = Memory::GetMapPoint( *iter );
        map_point->PrintInfo();
        LOG(INFO) << "observed in frame 1: "<< map_point->_obs[frameID1] << endl;
        LOG(INFO) << "observed in frame 2: "<< map_point->_obs[frameID2] << endl;
    }

    LOG(INFO) << endl;
    **/


    for ( auto iter = frame1->_obs.begin(); iter!=frame1->_obs.end(); iter++ )
    {
        MapPoint* map_point = Memory::GetMapPoint ( iter->first );
        if ( map_point==nullptr && map_point->_bad==true )
        {
            continue;
        }

        VertexSBAPointXYZ* pt_xyz = new VertexSBAPointXYZ();
        pt_xyz->setId ( pts_id++ );
        pt_xyz->setEstimate ( map_point->_pos_world );
        pt_xyz->setMarginalized ( true );
        optimizer.addVertex ( pt_xyz );
        vertex_points[map_point->_id] = pt_xyz;

        EdgeSophusSE3ProjectXYZ* edge1 = new EdgeSophusSE3ProjectXYZ();
        edge1->setVertex ( 0, pt_xyz );
        edge1->setVertex ( 1, v1 );

        edge1->setMeasurement ( frame1->_camera->Pixel2Camera2D ( map_point->_obs[frameID1].head<2>() ) );
        edge1->setInformation ( Eigen::Matrix2d::Identity() );
        // robust kernel ?
        optimizer.addEdge ( edge1 );
        edges.push_back ( edge1 );

        EdgeSophusSE3ProjectXYZ* edge2 = new EdgeSophusSE3ProjectXYZ();
        edge2->setVertex ( 0, pt_xyz );
        edge2->setVertex ( 1, v2 );
        edge2->setMeasurement ( frame2->_camera->Pixel2Camera2D ( map_point->_obs[frameID2].head<2>() ) );
        edge2->setInformation ( Eigen::Matrix2d::Identity() );
        // robust kernel ?
        optimizer.addEdge ( edge2 );
        edges.push_back ( edge2 );
    }

    LOG ( INFO ) << "edges: "<<edges.size() <<endl;

    // do optimization!  >_<
    optimizer.initializeOptimization();
    // optimizer.computeActiveErrors();
    // LOG(INFO) << "initial error: " << optimizer.activeChi2() << endl;
    optimizer.optimize ( 10 );
    // optimizer.computeActiveErrors();
    // LOG(INFO) << "optimized error: " << optimizer.activeChi2() << endl;

    // update the key-frame and map points
    // TODO delete the outlier! 但是outlier应该在前面的估计中去过一次了啊
    // LOG(INFO) << "frame 2 before optimization: \n" << frame2->_T_c_w.matrix()<<endl;
    frame1->_T_c_w = SE3::exp ( v1->estimate() );
    frame2->_T_c_w = SE3::exp ( v2->estimate() );
    // LOG(INFO) << "frame 2 after optimization: \n" << frame2->_T_c_w.matrix()<<endl;

    for ( auto v:vertex_points )
    {
        MapPoint* map_point = Memory::GetMapPoint ( v.first );
        map_point->_pos_world = v.second->estimate();
    }
}

// 日，Ceres会修改scale……
void TwoViewBACeres (
    const long unsigned int& frameID1,
    const long unsigned int& frameID2, 
    bool robust
)
{
    Frame* frame1 = Memory::GetFrame ( frameID1 );
    Frame* frame2 = Memory::GetFrame ( frameID2 );

    Vector6d pose2;
    Vector3d r2 = frame2->_T_c_w.so3().log(), t2=frame2->_T_c_w.translation();
    pose2.head<3>() = t2;
    pose2.tail<3>() = r2;

    ceres::Problem problem;
    
    auto& all_points = Memory::GetAllPoints();
    for ( auto& map_point_pair: all_points ) {
        MapPoint* map_point = map_point_pair.second;
        for ( auto obs:map_point->_obs )
        {
            if ( obs.first == frame1->_id ) {
                Vector2d pt = frame1->_camera->Pixel2Camera2D ( obs.second.head<2>() );
                // 不想要改第一帧的位姿
                problem.AddResidualBlock (
                    new ceres::AutoDiffCostFunction<CeresReprojectionErrorPointOnly,2,3> (
                        new CeresReprojectionErrorPointOnly ( pt, frame1->_T_c_w )
                    ),
                    robust? new ceres::HuberLoss(5):nullptr,
                    map_point->_pos_world.data()
                );
            } else {
                Vector2d pt = frame2->_camera->Pixel2Camera2D ( obs.second.head<2>() );
                problem.AddResidualBlock (
                    new ceres::AutoDiffCostFunction<CeresReprojectionError,2,6,3> (
                        new CeresReprojectionError( pt )
                    ),
                    robust? new ceres::HuberLoss(5):nullptr,
                    pose2.data(),
                    map_point->_pos_world.data()
                );
            }
        }
    }
    
    ceres::Solver::Options options;
    options.linear_solver_type = ceres::DENSE_SCHUR;
    // options.minimizer_progress_to_stdout = true;

    ceres::Solver::Summary summary;
    ceres::Solve ( options, &problem, &summary );
    // cout<< summary.FullReport() << endl;

    // set the value of the second frame
    frame2->_T_c_w = SE3 (
                         SO3::exp ( pose2.tail<3>() ), pose2.head<3>()
                     );
    
    // check the depth in the two views 
    /*
    for ( auto iter = frame1->_map_point.begin(); iter!= frame1->_map_point.end(); iter++ )
    {
        MapPoint::Ptr map_point = Memory::GetMapPoint ( *iter );
        if ( map_point->_bad ) continue;
        
        Vector3d pt1 = frame1->_camera->World2Camera( map_point->_pos_world, frame1->_T_c_w );
        Vector3d pt2 = frame1->_camera->World2Camera( map_point->_pos_world, frame1->_T_c_w );
        if ( pt1[2] < 0.05 || pt1[2]>15 || pt2[2] < 0.05 || pt2[2]>15 )  {
            // invalid depth 
            map_point->_bad = true; 
        }
    }
    */
}

void SparseImgAlign::SparseImageAlignmentCeres (
    Frame* frame1, Frame* frame2,
    const int& pyramid_level
)
{
    _pyramid_level = pyramid_level;
    _scale = 1<<pyramid_level;
    if ( frame1 == _frame1 )
    {
        // 没必要重新算 ref 的 patch
        _have_ref_patch = true;
    } else {
        _have_ref_patch = false;
    }

    _frame1 = frame1;
    _frame2 = frame2;

    // cv::Mat& curr_img = _frame2->_pyramid[pyramid_level];
    if ( _have_ref_patch==false )
    {
        PrecomputeReferencePatches();
        //LOG ( INFO ) <<"ref patterns: "<<_patterns_ref.size() <<endl;
    }

    // solve this problem
    ceres::Problem problem;
    Vector6d pose2;
    Vector3d r2 = _TCR.so3().log(), t2=_TCR.translation();
    pose2.head<3>() = t2;
    pose2.tail<3>() = r2;

    int index = 0;
    for ( auto it=_frame1->_obs.begin(); it!=_frame1->_obs.end(); it++, index++ )
    {
        // camera coordinates in ref
        if ( Memory::GetMapPoint(it->first)->_bad == true ) continue;
        
        Vector3d xyz_ref = _frame1->_camera->Pixel2Camera( it->second.head<2>() ,it->second[2]) ; // 我日，之前忘乘了深度，怪不得怎么算都不对
        problem.AddResidualBlock (
            new CeresReprojSparseDirectError (
                _frame2->_pyramid[_pyramid_level],
                _patterns_ref[index],
                xyz_ref,
                _frame1->_camera,
                _scale
            ),
            // new ceres::HuberLoss(10), // TODO do I need Loss Function?
            nullptr,
            pose2.data()
        );
    }

    ceres::Solver::Options options;
    options.linear_solver_type = ceres::DENSE_SCHUR;
    // options.minimizer_progress_to_stdout = true;

    ceres::Solver::Summary summary;
    ceres::Solve ( options, &problem, &summary );
    // cout<< summary.FullReport() << endl;
    // show the estimated pose
    _TCR = SE3 (
               SO3::exp ( pose2.tail<3>() ),
               pose2.head<3>()
           );

    /*
#ifdef DEBUG_VIZ
    cv::Mat ref_show, curr_show;
    cv::cvtColor ( _frame1->_pyramid[pyramid_level], ref_show, CV_GRAY2BGR );
    cv::cvtColor ( _frame2->_pyramid[pyramid_level], curr_show, CV_GRAY2BGR );

    LOG ( INFO ) << "TCR = " << _TCR.matrix() << endl;
    auto iter_mp = _frame1->_map_point.begin(); 
    auto iter_obs = _frame1->_observations.begin(); 
    for ( ; iter_mp!=_frame1->_map_point.end(); iter_mp++, iter_obs++ )
    {
        // camera coordinates in ref
        Vector2d px_ref = iter_obs->head<2>();
        Vector3d pt_ref = _frame1->_camera->Pixel2Camera ( px_ref, (*iter_obs)[2]);
        // in current
        Vector3d xyz_curr = _TCR * pt_ref;
        Vector2d px_curr = _frame2->_camera->Camera2Pixel ( xyz_curr ) / _scale;

        cv::circle ( ref_show, cv::Point2d ( px_ref[0]/_scale, px_ref[1]/_scale ), 3, cv::Scalar ( 0,250,0 ) );
        cv::circle ( curr_show, cv::Point2d ( px_curr[0], px_curr[1] ), 3, cv::Scalar ( 0,250,0 ) );
    }
    cv::imshow ( "ref", ref_show );
    cv::imshow ( "curr", curr_show );
    cv::waitKey ( 0 );
#endif
    */
}


void SparseImgAlign::PrecomputeReferencePatches()
{
    _patterns_ref.resize ( _frame1->_obs.size() );

    cv::Mat& ref_img = _frame1->_pyramid[_pyramid_level];
    int i=0;
    int scale = 1<<_pyramid_level;
    for( auto it_obs = _frame1->_obs.begin(); it_obs != _frame1->_obs.end(); it_obs++, i++ ) {
        MapPoint* mp = Memory::GetMapPoint( it_obs->first );
        if ( mp->_bad ) continue; 
        Vector2d px_ref = it_obs->second.head<2>() * 1.0/scale;
        PixelPattern pattern_ref;
        for ( int k=0; k<PATTERN_SIZE; k++ )
        {
            double u = px_ref[0] + PATTERN_DX[k];
            double v = px_ref[1] + PATTERN_DX[k];
            pattern_ref.pattern[k] = utils::GetBilateralInterp ( u,v,ref_img );
        }
        _patterns_ref[i] = pattern_ref;
    }
    _have_ref_patch = true;
}

void OptimizePoseCeres ( Frame* current, bool robust )
{
    Vector6d pose;
    Vector3d r = current->_T_c_w.so3().log(), t = current->_T_c_w.translation();
    pose.head<3>() = t;
    pose.tail<3>() = r;

    ceres::Problem problem;
    for ( auto iter = current->_obs.begin(); iter!=current->_obs.end(); iter++ )
    {
        MapPoint* map_point = Memory::GetMapPoint ( iter->first );
        if ( iter->second[2] < 0 )
        {
            continue;
        }
        if ( robust ) {
            problem.AddResidualBlock (
                new ceres::AutoDiffCostFunction< CeresReprojectionErrorPoseOnly, 2, 6> (
                    new CeresReprojectionErrorPoseOnly (
                        current->_camera->Pixel2Camera2D ( iter->second.head<2>() ),
                        map_point->_pos_world
                    )
                ),
                new ceres::HuberLoss ( 5 ),
                pose.data()
            );
        } else {
            problem.AddResidualBlock (
                new ceres::AutoDiffCostFunction< CeresReprojectionErrorPoseOnly, 2, 6> (
                    new CeresReprojectionErrorPoseOnly (
                        current->_camera->Pixel2Camera2D ( iter->second.head<2>() ),
                        map_point->_pos_world
                    )
                ),
                nullptr,
                pose.data()
            );
            
        }
    }

    ceres::Solver::Options options;
    options.linear_solver_type = ceres::DENSE_SCHUR;
    // options.minimizer_progress_to_stdout = true;

    ceres::Solver::Summary summary;
    ceres::Solve ( options, &problem, &summary );
    // cout<< summary.FullReport() << endl;

    // set the pose of current
    current->_T_c_w = SE3 (
                          SO3::exp ( pose.tail<3>() ), pose.head<3>()
                      );
}

/***************************************************
 * Depth filter
 * *************************************************/

Seed::Seed ( const unsigned long& frame, cv::KeyPoint& keypoint, float depth_mean, float depth_min ) :
    id ( seed_counter++ ),
    kp ( keypoint ),
    a ( 10 ),
    b ( 10 ),
    mu ( 1.0/depth_mean ),
    z_range ( 1.0/depth_min ),
    sigma2 ( z_range*z_range/36 ),
    frame_id( frame )
{
}

int Seed::batch_counter =0;
int Seed::seed_counter =0;

void DepthFilter::AddKeyframe ( Frame* frame, double depth_mean, double depth_min )
{
    LOG(INFO) << "mean depth = " << depth_mean << ", min depth = " << depth_min << endl;
    _new_keyframe_set = true;
    _new_keyframe_min_depth = depth_min;
    _new_keyframe_mean_depth = depth_mean;
    
    _frame_queue.push_back( frame );
    InitializeSeeds( frame );
    
    Seed::batch_counter++;
}

void DepthFilter::InitializeSeeds ( Frame* frame )
{
    // frame 的 map point candidate 已经被特征提取算法提取了
    for ( cv::KeyPoint& kp: frame->_map_point_candidates )
    {
        _seeds.push_back ( 
            Seed ( frame->_id ,kp, _new_keyframe_mean_depth, _new_keyframe_min_depth ) 
        );
    }

    if ( _options.verbose )
    {
        LOG ( INFO ) << "Depth filer initialized with seeds: "<<_seeds.size() << endl;
    }
}


// why remove key-frame ...
void DepthFilter::RemoveKeyframe ( Frame* frame )
{
    list<Seed>::iterator it=_seeds.begin();
    size_t n_removed = 0;
    while ( it!=_seeds.end() )
    {
        if ( it->frame_id == frame->_id )
        {
            it = _seeds.erase ( it );
            ++n_removed;
        }
        else
        {
            ++it;
        }
    }
    LOG ( INFO ) << "removed "<<n_removed <<" seeds."<<endl;
}

void DepthFilter::ClearFrameQueue()
{
    _frame_queue.clear();
}

void DepthFilter::AddFrame ( Frame* frame )
{
    /*
    _frame_queue.push_back ( frame );
    if ( _frame_queue.size() > 2 )
    {
        _frame_queue.pop_front();
    }
    */

    // 根据普通帧信息去更新关键帧的 seeds
    UpdateSeeds ( frame );
    
    /*
    // display the seeds 
#ifdef DEBUG_VIZ 
    boost::format fmt("%3.2f,%3.2f");
    cv::Mat img_ref = _frame_queue.back()->_color.clone();
    for ( Seed& seed:_seeds ) {
        Vector2d px = seed.ftr->_obs[ _frame_queue.back()->_id ].head<2>();
        cv::circle( img_ref, cv::Point2f(px[0], px[1]), 5, cv::Scalar(0,250*seed.mu/10,250*seed.mu/10) );
        
        // string str = (fmt%seed.mu%seed.sigma2).str();
        // cv::putText( img_ref, str, cv::Point2f(px[0], px[1]-15), cv::HersheyFonts::FONT_HERSHEY_PLAIN, 1, cv::Scalar(0,250,0), 1 );
    }
    
    cv::imshow("depth filter", img_ref );
    cv::waitKey(0);
#endif
    */
}


void DepthFilter::UpdateSeedsLoop()
{
    // 这应该是个循环队列的线程
}

void DepthFilter::UpdateSeeds ( Frame* frame )
{
    // update only a limited number of seeds, because we don't have time to do it
    // for all the seeds in every frame!
    size_t n_updates=0, n_failed_matches=0, n_seeds = _seeds.size();
    list<Seed>::iterator it=_seeds.begin();

    const double focal_length = frame->_camera->focal();
    double px_noise = 1.0;
    double px_error_angle = atan ( px_noise/ ( 2.0*focal_length ) ) *2.0; // law of chord (sehnensatz)

    int cntSucceed = 0, cntFailed = 0;
    while ( it!=_seeds.end() )
    {
        // check if seed is not already too old
        if ( ( Seed::batch_counter - it->frame_id ) > _options.max_n_kfs )
        {
            it = _seeds.erase ( it );
            continue;
        }
        /*
        if ( it->b > 25 ) {
            it = _seeds.erase( it );
            continue; 
        }
        */

        // check if point is visible in the current image
        Frame* first_frame = Memory::GetFrame ( it->frame_id );
        SE3 T_ref_cur = first_frame->_T_c_w * frame->_T_c_w.inverse();

        Vector3d pt_ref = frame->_camera->Pixel2Camera ( utils::Cv2Eigen(it->kp.pt) );

        // xyz in current
        Vector3d xyz_f ( T_ref_cur.inverse() * ( 1.0/it->mu * pt_ref ) );
        if ( xyz_f.z() < 0.0 )
        {
            ++it; // behind the camera
            cntFailed++;
            continue;
        }
        
        if ( !frame->InFrame ( frame->_camera->Camera2Pixel ( xyz_f ) ) )
        {
            ++it; // point does not project in image
            cntFailed++;
            continue;
        }

        // we are using inverse depth coordinates
        float z_inv_min = it->mu + sqrt ( it->sigma2 );
        float z_inv_max = max ( it->mu - sqrt ( it->sigma2 ), 0.00000001f );
        double z=0;
        Vector2d matched_px;
        if ( !FindEpipolarMatchDirect ( first_frame, frame, it->kp, 0.9/it->mu, 1.1/z_inv_min, 1.0/z_inv_max, z, matched_px) ) // 稍微扩大一点搜索范围试试
        {
            // 挂了
            // LOG(INFO) << "find epipolar match failed"<<endl;
            /*
            Mat ref_show = first_frame->_color.clone();
            Mat curr_show = frame->_color.clone();
            cv::circle( ref_show, it->kp.pt, 2, cv::Scalar(0,0,250), 2 );
            cv::circle( curr_show, cv::Point2f(matched_px[0], matched_px[1]), 2, cv::Scalar(0,0,250), 2 );
            cv::imshow("wrong seed ref", ref_show);
            cv::imshow("wrong seed curr", curr_show);
            cv::waitKey(0);
            */
            // it->b++;
            ++it;
            ++n_failed_matches;
            cntFailed++;
            continue;
        }
        else {
            /*
            LOG(INFO) << "find epipolar match success"<<endl;
            LOG(INFO) << "depth changed from " << 1.0/it->mu << " to " << z << endl;
            Mat ref_show = first_frame->_color.clone();
            Mat curr_show = frame->_color.clone();
            cv::circle( ref_show, it->kp.pt, 2, cv::Scalar(0,250,0), 2 );
            cv::circle( curr_show, cv::Point2f(matched_px[0], matched_px[1]), 2, cv::Scalar(0,250,0), 2 );
            cv::imshow("correct seed ref", ref_show);
            cv::imshow("correct seed curr", curr_show);
            cv::waitKey(0);
            */
        }

        cntSucceed ++;
        // compute tau
        double tau = ComputeTau ( T_ref_cur, pt_ref, z, px_error_angle );
        double tau_inverse = 0.5 * ( 1.0/max ( 0.0000001, z-tau ) - 1.0/ ( z+tau ) );

        // update the estimate
        UpdateSeed ( 1./z, tau_inverse*tau_inverse, &*it );
        ++n_updates;

        // if the seed has converged, we initialize a new candidate point and remove the seed
        if ( sqrt ( it->sigma2 ) < it->z_range/_options.seed_convergence_sigma2_thresh )
        {
            // 向 Memory 注册一个新的 Map Point，并且添加到 Local Map 中
            
            it->PrintInfo();
            
            MapPoint* mp = Memory::CreateMapPoint();
            Vector3d xyz_world ( frame->_T_c_w.inverse() * ( pt_ref * ( 1.0/it->mu ) ) );
            
            mp->_pos_world = xyz_world; 
            mp->_pyramid_level = it->kp.octave;
            mp->_first_observed_frame = it->frame_id;
            mp->_obs[it->frame_id].head<2>() = utils::Cv2Eigen( it->kp.pt );
            mp->_obs[it->frame_id][2] = 1.0/it->mu;
            frame->_obs[mp->_id] = mp->_obs[it->frame_id];
            mp->_converged = false; 
            mp->_bad = false;
            
            LOG(INFO) << "seed "<< mp->_id <<" from key-frame " << it->frame_id <<" has converged, depth = "<< 1.0/it->mu 
                << ", pos = "<<xyz_world.transpose()<<", as map point "<<mp->_id<<endl;
                
            Mat ref_show = first_frame->_color.clone();
            Mat curr_show = frame->_color.clone();
            cv::circle( ref_show, it->kp.pt, 5, cv::Scalar(0,250,0), 2 );
            cv::circle( curr_show, cv::Point2f(matched_px[0], matched_px[1]), 5, cv::Scalar(0,250,0), 2 );
            cv::imshow("seed_ref", ref_show);
            cv::imshow("seed_curr", curr_show);
            cv::waitKey(0);
                
            _local_mapping->AddMapPoint( mp->_id );
            it = _seeds.erase ( it );
            
        }
        else if ( isnan ( z_inv_min ) )
        {
            it = _seeds.erase ( it );
        }
        else
        {
            ++it;
        }
    }
    
    LOG(INFO) << "seed succeed = "<< cntSucceed <<", failed = "<<cntFailed << endl;
}

void DepthFilter::UpdateSeed (
    const float& x, const float& tau2, Seed* seed )
{

    float norm_scale = sqrt ( seed->sigma2 + tau2 );
    if ( std::isnan ( norm_scale ) )
    {
        return;
    }
    // seed->PrintInfo();
    boost::math::normal_distribution<float> nd ( seed->mu, norm_scale );
    float s2 = 1./ ( 1./seed->sigma2 + 1./tau2 );
    float m = s2* ( seed->mu/seed->sigma2 + x/tau2 );
    float C1 = seed->a/ ( seed->a+seed->b ) * boost::math::pdf ( nd, x );
    float C2 = seed->b/ ( seed->a+seed->b ) * 1./seed->z_range;
    float normalization_constant = C1 + C2;
    C1 /= normalization_constant;
    C2 /= normalization_constant;
    float f = C1* ( seed->a+1. ) / ( seed->a+seed->b+1. ) + C2*seed->a/ ( seed->a+seed->b+1. );
    float e = C1* ( seed->a+1. ) * ( seed->a+2. ) / ( ( seed->a+seed->b+1. ) * ( seed->a+seed->b+2. ) )
              + C2*seed->a* ( seed->a+1.0f ) / ( ( seed->a+seed->b+1.0f ) * ( seed->a+seed->b+2.0f ) );

    // update parameters
    float mu_new = C1*m+C2*seed->mu;
    seed->sigma2 = C1* ( s2 + m*m ) + C2* ( seed->sigma2 + seed->mu*seed->mu ) - mu_new*mu_new;
    seed->mu = mu_new;
    seed->a = ( e-f ) / ( f-e/f );
    seed->b = seed->a* ( 1.0f-f ) /f;
    // LOG(INFO) << "updated: "; 
    // seed->PrintInfo();
    // LOG(INFO)<<endl;
}


double DepthFilter::ComputeTau (
    const SE3& T_ref_cur,
    const Vector3d& f,
    const double& z,
    const double& px_error_angle )
{
    Vector3d t ( T_ref_cur.translation() );
    Vector3d a = f*z-t;
    double t_norm = t.norm();
    double a_norm = a.norm();
    double alpha = acos ( f.dot ( t ) /t_norm ); // dot product
    double beta = acos ( a.dot ( -t ) / ( t_norm*a_norm ) ); // dot product
    double beta_plus = beta + px_error_angle;
    double gamma_plus = M_PI-alpha-beta_plus; // triangle angles sum to PI
    double z_plus = t_norm*sin ( beta_plus ) /sin ( gamma_plus ); // law of sines
    return ( z_plus - z ); // tau
}

void DepthFilter::GetSeedsCopy ( Frame* frame, list< Seed >& seeds )
{

}

void DepthFilter::Reset()
{

}
}
}
