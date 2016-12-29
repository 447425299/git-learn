#ifndef YGZ_MEMORY_H_
#define YGZ_MEMORY_H_

#include "ygz/frame.h"
#include "ygz/map_point.h"

namespace ygz {
    
class Viewer;
class VisualOdometry;
    
// 内存管理类
// 它存储了所有的关键帧和关键帧带着的地图点，也只有这些帧的id是合法的，可以从memory中拿出来
// 单件，不允许复制
class Memory {
    friend Viewer;
    friend VisualOdometry;
    
public:
    Memory( const Memory& ) =delete;
    Memory& operator = ( const Memory& ) =delete;
    
    ~Memory() { 
        if ( _mem != nullptr )
            _mem->clean();
    }
    
    static Frame* CreateNewFrame(); 
    
    static Frame* CreateNewFrame(
        const double& timestamp, 
        const SE3& T_c_w, 
        const bool is_keyframe, 
        const Mat& color, 
        const Mat& depth = Mat()
    ); 
    
    // register a frame into memory, assign an ID with it
    // if already registered, you can choose whether to overwrite the exist one 
    static Frame* RegisterFrame( Frame* frame, bool overwrite=false );
    
    static MapPoint* CreateMapPoint(); 
    
    static inline Frame* GetFrame( const unsigned long& id ) {
        auto iter = _frames.find( id );
        if ( iter == _frames.end() )
            return nullptr;
        return iter->second;
    }
    
    static inline MapPoint* GetMapPoint( const unsigned long& id ) {
        auto iter = _points.find( id );
        if ( iter == _points.end() )
            return nullptr;
        return iter->second;
    }
    
    static inline unordered_map<unsigned long, MapPoint*> & GetAllPoints() {
        return _points;
    }
    
    void optimizeMemory(); 
    void clean();
    
    /*
    static void PrintInfo() { 
        for ( auto& frame_pair: _frames) {
            LOG(INFO) << "frame "<<frame_pair.first<<endl;
        }
    }
    */
        
    
private:
    Memory() {}
    
private:
    static unordered_map<unsigned long, Frame*>         _frames; 
    static unordered_map<unsigned long, MapPoint*>      _points;
    static unsigned long _id_frame, _id_points; 
    static shared_ptr<Memory> _mem;
};
    
}

#endif
