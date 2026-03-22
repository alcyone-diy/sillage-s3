#ifndef TILE_ENGINE_HPP
#define TILE_ENGINE_HPP

#include "lvgl.h"
#include <vector>
#include <string>

#define TILE_DEBUG 1

class TileEngine {
public:
    TileEngine();
    ~TileEngine();

    void init();
    void setMapCenter(double lat, double lon, int zoom);
    void debug(double lat, double lon, int zoom);

    static void lv_rgb565_decoder_init();
    static void lv_jpeg_esp_decoder_init();
    static void initImageDecoders();

private:
    struct TileInfo {
        lv_obj_t* img_obj;
        int x_idx;
        int y_idx;
        int zoom;
        char path[64];
    };

    static constexpr int TILE_SIZE = 256;
    static constexpr int GRID_COLS = 5;
    static constexpr int GRID_ROWS = 3;
    static constexpr int SCREEN_WIDTH = 800;
    static constexpr int SCREEN_HEIGHT = 480;
    static constexpr const char* TILE_PATH_BASE = "/tiles";
    static constexpr const char* LV_DRIVE_PREFIX = "S:";

    lv_obj_t* _map_container;
    lv_obj_t* _tile_layer;
    std::vector<TileInfo> _tile_grid;

    double _current_lat = 0;
    double _current_lon = 0;
    int _current_zoom = 8;
    int _min_zoom = 1;
    int _max_zoom = 18;
    int _base_tile_x = -1;
    int _base_tile_y = -1;
    lv_point_t _last_drag_point = {0, 0};
    double _last_pinch_dist = 0;
    bool _is_pinching = false;
    uint32_t _last_zoom_time = 0;

    void updateTiles(double lat, double lon, int zoom);
    void latLonToTile(double lat, double lon, int zoom, double& x, double& y);
    void tileToLatLon(double x, double y, int zoom, double& lat, double& lon);
    void getTilePath(char* buf, size_t buf_size, int zoom, int x, int y, bool for_lvgl);
    void loadConfig();
    static void event_handler(lv_event_t* e);
};

#endif // TILE_ENGINE_HPP
