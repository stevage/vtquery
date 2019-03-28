#include "vtquery.hpp"
#include "util.hpp"

#include <algorithm>
#include <exception>
#include <gzip/decompress.hpp>
#include <gzip/utils.hpp>
#include <mapbox/geometry/algorithms/closest_point.hpp>
#include <mapbox/geometry/algorithms/closest_point_impl.hpp>
#include <mapbox/geometry/geometry.hpp>
#include <mapbox/vector_tile.hpp>
#include <memory>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vtzero/types.hpp>
#include <vtzero/vector_tile.hpp>

namespace VectorTileQuery {

enum GeomType { point,
                linestring,
                polygon,
                all,
                unknown };
static const char* GeomTypeStrings[] = {"point", "linestring", "polygon", "unknown"};
const char* getGeomTypeString(int enumVal) {
    return GeomTypeStrings[enumVal];
}

using materialized_prop_type = std::pair<std::string, mapbox::feature::value>;

/// main storage item for returning to the user
struct ResultObject {
    std::vector<vtzero::property> properties_vector;
    std::vector<materialized_prop_type> properties_vector_materialized;
    std::string layer_name;
    mapbox::geometry::point<double> coordinates;
    double distance;
    GeomType original_geometry_type{GeomType::unknown};
    bool has_id{false};
    uint64_t id{0};

    ResultObject() : coordinates(0.0, 0.0),
                     distance(std::numeric_limits<double>::max()) {}

    ResultObject(ResultObject&&) = default;
    ResultObject& operator=(ResultObject&&) = default;
    ResultObject(ResultObject const&) = delete;
    ResultObject& operator=(ResultObject const&) = delete;
};

/// an intermediate representation of a tile buffer and its necessary components
struct TileObject {
    TileObject(std::int32_t z0,
               std::int32_t x0,
               std::int32_t y0,
               Napi::Buffer<char> const& buffer)
        : z(z0),
          x(x0),
          y(y0),
          data{buffer.Data(), buffer.Length()},
          buffer_ref{Napi::Persistent(buffer)} {
    }

    // explicitly use the destructor to clean up
    // the persistent buffer ref by Reset()-ing
    ~TileObject() {
        buffer_ref.Reset();
    }

    // guarantee that objects are not being copied by deleting the
    // copy and move definitions

    // non-copyable
    TileObject(TileObject const&) = delete;
    TileObject& operator=(TileObject const&) = delete;

    // non-movable
    TileObject(TileObject&&) = delete;
    TileObject& operator=(TileObject&&) = delete;

    std::int32_t z;
    std::int32_t x;
    std::int32_t y;
    vtzero::data_view data;
    Napi::Reference<Napi::Buffer<char>> buffer_ref;
};

/// the baton of data to be passed from the v8 thread into the cpp threadpool
struct QueryData {
    explicit QueryData(std::uint32_t num_tiles)
        : latitude(0.0),
          longitude(0.0),
          radius(0.0),
          num_results(5),
          dedupe(true),
          geometry_filter_type(GeomType::all) {
        tiles.reserve(num_tiles);
    }

    // non-copyable
    QueryData(QueryData const&) = delete;
    QueryData& operator=(QueryData const&) = delete;

    // non-movable
    QueryData(QueryData&&) = delete;
    QueryData& operator=(QueryData&&) = delete;

    // buffers object thing
    std::vector<std::unique_ptr<TileObject>> tiles;
    std::vector<std::string> layers;
    double latitude;
    double longitude;
    double radius;
    std::uint32_t num_results;
    bool dedupe;
    GeomType geometry_filter_type;
};

/// convert properties to v8 types
struct property_value_visitor {
    Napi::Object& properties_obj;
    std::string const& key;
    Napi::Env& env;
    template <typename T>
    void operator()(T const& /*unused*/) {
    }

    void operator()(bool v) {
        properties_obj.Set(key, Napi::Boolean::New(env, v));
    }
    void operator()(uint64_t v) {
        properties_obj.Set(key, Napi::Number::New(env, v));
    }
    void operator()(int64_t v) {
        properties_obj.Set(key, Napi::Number::New(env, v));
    }
    void operator()(double v) {
        properties_obj.Set(key, Napi::Number::New(env, v));
    }
    void operator()(std::string const& v) {
        properties_obj.Set(key, Napi::String::New(env, v));
    }
};

/// used to create the final v8 (JSON) object to return to the user
void set_property(materialized_prop_type const& property,
                  Napi::Object& properties_obj, Napi::Env env) {
    mapbox::util::apply_visitor(property_value_visitor{properties_obj, property.first, env}, property.second);
}

GeomType get_geometry_type(vtzero::feature const& f) {
    GeomType gt = GeomType::unknown;
    switch (f.geometry_type()) {
    case vtzero::GeomType::POINT: {
        gt = GeomType::point;
        break;
    }
    case vtzero::GeomType::LINESTRING: {
        gt = GeomType::linestring;
        break;
    }
    case vtzero::GeomType::POLYGON: {
        gt = GeomType::polygon;
        break;
    }
    default: {
        break;
    }
    }

    return gt;
}

struct CompareDistance {
    bool operator()(ResultObject const& r1, ResultObject const& r2) {
        return r1.distance < r2.distance;
    }
};

/// replace already existing results with a better, duplicate result
void insert_result(ResultObject& old_result,
                   std::vector<vtzero::property>& props_vec,
                   std::string const& layer_name,
                   mapbox::geometry::point<double>& pt,
                   double distance,
                   GeomType geom_type,
                   bool has_id,
                   uint64_t id) {

    std::swap(old_result.properties_vector, props_vec);
    old_result.layer_name = layer_name;
    old_result.coordinates = pt;
    old_result.distance = distance;
    old_result.original_geometry_type = geom_type;
    old_result.has_id = has_id;
    old_result.id = id;
}

/// generate a vector of vtzero::property objects
std::vector<vtzero::property> get_properties_vector(vtzero::feature& f) {
    std::vector<vtzero::property> v;
    v.reserve(f.num_properties());
    while (auto ii = f.next_property()) {
        v.push_back(ii);
    }
    return v;
}

/// compare two features to determine if they are duplicates
bool value_is_duplicate(ResultObject const& r,
                        vtzero::feature const& candidate_feature,
                        std::string const& candidate_layer,
                        GeomType const candidate_geom,
                        std::vector<vtzero::property> const& candidate_props_vec) {

    // compare layer (if different layers, not duplicates)
    if (r.layer_name != candidate_layer) {
        return false;
    }

    // compare geometry (if different geometry types, not duplicates)
    if (r.original_geometry_type != candidate_geom) {
        return false;
    }

    // compare ids
    if (r.has_id && candidate_feature.has_id() && r.id != candidate_feature.id()) {
        return false;
    }

    // compare property tags
    return r.properties_vector == candidate_props_vec;
}

struct Worker : Napi::AsyncWorker {
    using Base = Napi::AsyncWorker;

    /// set up major containers
    std::unique_ptr<QueryData> query_data_;
    std::vector<ResultObject> results_queue_;

    Worker(std::unique_ptr<QueryData>&& query_data, Napi::Function& cb)
        : Base(cb, "vtquery:worker"),
          query_data_(std::move(query_data)) {}

    void Execute() override {
        try {
            QueryData const& data = *query_data_;

            // reserve the query results and fill with empty objects
            results_queue_.reserve(data.num_results);
            for (std::size_t i = 0; i < data.num_results; ++i) {
                results_queue_.emplace_back();
            }

            // query point lng/lat geometry.hpp point (used for distance calculation later on)
            mapbox::geometry::point<double> query_lnglat{data.longitude, data.latitude};

            gzip::Decompressor decompressor;
            std::string uncompressed;
            std::vector<std::string> buffers;
            std::vector<std::tuple<vtzero::vector_tile, std::int32_t, std::int32_t, std::int32_t>> tiles;
            tiles.reserve(data.tiles.size());
            for (auto const& tile_ptr : data.tiles) {
                TileObject const& tile_obj = *tile_ptr;
                if (gzip::is_compressed(tile_obj.data.data(), tile_obj.data.size())) {
                    decompressor.decompress(uncompressed, tile_obj.data.data(), tile_obj.data.size());
                    buffers.emplace_back(std::move(uncompressed));
                    tiles.emplace_back(vtzero::vector_tile(buffers.back()), tile_obj.z, tile_obj.x, tile_obj.y);
                } else {
                    tiles.emplace_back(vtzero::vector_tile(tile_obj.data), tile_obj.z, tile_obj.x, tile_obj.y);
                }
            }
            // for each tile
            for (auto& tile_obj : tiles) {
                vtzero::vector_tile& tile = std::get<0>(tile_obj);
                while (auto layer = tile.next_layer()) {

                    // check if this is a layer we should query
                    std::string layer_name = std::string(layer.name());

                    if (!data.layers.empty() && std::find(data.layers.begin(), data.layers.end(), layer_name) == data.layers.end()) {
                        continue;
                    }
                    std::uint32_t extent = layer.extent();
                    std::int32_t tile_obj_z = std::get<1>(tile_obj);
                    std::int32_t tile_obj_x = std::get<2>(tile_obj);
                    std::int32_t tile_obj_y = std::get<3>(tile_obj);
                    // query point in relation to the current tile the layer extent
                    mapbox::geometry::point<std::int64_t> query_point = utils::create_query_point(data.longitude, data.latitude, extent, tile_obj_z, tile_obj_x, tile_obj_y);

                    while (auto feature = layer.next_feature()) {
                        auto original_geometry_type = get_geometry_type(feature);

                        // check if this a geometry type we want to keep
                        if (data.geometry_filter_type != GeomType::all && data.geometry_filter_type != original_geometry_type) {
                            continue;
                        }

                        // implement closest point algorithm on query geometry and the query point
                        auto const cp_info = mapbox::geometry::algorithms::closest_point(mapbox::vector_tile::extract_geometry<int64_t>(feature), query_point);

                        // distance should never be less than zero, this is a safety check
                        if (cp_info.distance < 0.0) {
                            continue;
                        }

                        double meters = 0.0;
                        auto ll = mapbox::geometry::point<double>{data.longitude, data.latitude}; // default to original query lng/lat

                        // if distance from the query point is greater than 0.0 (not a direct hit) so recalculate the latlng
                        if (cp_info.distance > 0.0) {
                            ll = utils::convert_vt_to_ll(extent, tile_obj_z, tile_obj_x, tile_obj_y, cp_info);
                            meters = utils::distance_in_meters(query_lnglat, ll);
                        }

                        // if distance from the query point is greater than the radius, don't add it
                        if (meters > data.radius) {
                            continue;
                        }

                        // check for duplicates
                        // if the candidate is a duplicate and smaller in distance, replace it
                        bool found_duplicate = false;
                        bool skip_duplicate = false;
                        auto properties_vec = get_properties_vector(feature);
                        if (data.dedupe) {
                            for (auto& result : results_queue_) {
                                if (value_is_duplicate(result, feature, layer_name, original_geometry_type, properties_vec)) {
                                    if (meters <= result.distance) {
                                        insert_result(result, properties_vec, layer_name, ll, meters, original_geometry_type, feature.has_id(), feature.id());
                                        found_duplicate = true;
                                        break;
                                        // if we have a duplicate but it's lesser than what we already have, just skip and don't add below
                                    }
                                    skip_duplicate = true;
                                    break;
                                }
                            }
                        }

                        if (skip_duplicate) {
                            continue;
                        }

                        if (found_duplicate) {
                            std::stable_sort(results_queue_.begin(), results_queue_.end(), CompareDistance());
                            continue;
                        }

                        if (meters < results_queue_.back().distance) {
                            insert_result(results_queue_.back(), properties_vec, layer_name, ll, meters, original_geometry_type, feature.has_id(), feature.id());
                            std::stable_sort(results_queue_.begin(), results_queue_.end(), CompareDistance());
                        }
                    } // end tile.layer.feature loop
                }     // end tile.layer loop
            }         // end tile loop
            // Here we create "materialized" properties. We do this here because, when reading from a compressed
            // buffer, it is unsafe to touch `feature.properties_vector` once we've left this loop.
            // That is because the buffer may represent uncompressed data that is not in scope outside of Execute()
            for (auto& feature : results_queue_) {
                feature.properties_vector_materialized.reserve(feature.properties_vector.size());
                for (auto const& property : feature.properties_vector) {
                    auto val = vtzero::convert_property_value<mapbox::feature::value, mapbox::vector_tile::detail::property_value_mapping>(property.value());
                    feature.properties_vector_materialized.emplace_back(std::string(property.key()), std::move(val));
                }
            }
        } catch (std::exception const& e) {
            SetError(e.what());
        }
    }

    void OnOK() override {
        Napi::HandleScope scope(Env());
        try {
            Napi::Object results_object = Napi::Object::New(Env());
            Napi::Array features_array = Napi::Array::New(Env());
            results_object.Set("type", "FeatureCollection");

            // for each result object
            while (!results_queue_.empty()) {
                auto const& feature = results_queue_.back(); // get reference to top item in results queue
                if (feature.distance < std::numeric_limits<double>::max()) {
                    // if this is a default value, don't use it
                    Napi::Object feature_obj = Napi::Object::New(Env());
                    feature_obj.Set("type", "Feature");
                    feature_obj.Set("id", feature.id);

                    // create geometry object
                    Napi::Object geometry_obj = Napi::Object::New(Env());
                    geometry_obj.Set("type", "Point");
                    Napi::Array coordinates_array = Napi::Array::New(Env(), 2);
                    coordinates_array.Set(0u, feature.coordinates.x); // latitude
                    coordinates_array.Set(1u, feature.coordinates.y); // longitude
                    geometry_obj.Set("coordinates", coordinates_array);
                    feature_obj.Set("geometry", geometry_obj);

                    // create properties object
                    Napi::Object properties_obj = Napi::Object::New(Env());
                    for (auto const& prop : feature.properties_vector_materialized) {
                        set_property(prop, properties_obj, Env());
                    }

                    // set properties.tilquery
                    Napi::Object tilequery_properties_obj = Napi::Object::New(Env());
                    tilequery_properties_obj.Set("distance", feature.distance);
                    std::string og_geom = getGeomTypeString(feature.original_geometry_type);
                    tilequery_properties_obj.Set("geometry", og_geom);
                    tilequery_properties_obj.Set("layer", feature.layer_name);
                    properties_obj.Set("tilequery", tilequery_properties_obj);

                    // add properties to feature
                    feature_obj.Set("properties", properties_obj);

                    // add feature to features array
                    features_array.Set(static_cast<uint32_t>(results_queue_.size() - 1), feature_obj);
                }

                results_queue_.pop_back();
            }

            results_object.Set("features", features_array);
            Callback().Call({Env().Null(), results_object});

        } catch (std::exception const& e) {
            // unable to create test to throw exception here, the try/catch is simply
            // for unexpected cases https://github.com/mapbox/vtquery/issues/69
            // LCOV_EXCL_START
            //auto const argc = 1u;
            //v8::Local<v8::Value> argv[argc] = {Nan::Error(e.what())};
            //callback->Call(argc, static_cast<v8::Local<v8::Value>*>(argv), async_resource);
            SetError(e.what());
            // LCOV_EXCL_STOP
        }
    }
};

Napi::Value vtquery(Napi::CallbackInfo const& info) {
    // validate callback function
    std::size_t length = info.Length();
    if (length == 0) {
        Napi::Error::New(info.Env(), "last argument must be a callback function").ThrowAsJavaScriptException();
        return info.Env().Null();
    }
    Napi::Value callback_val = info[length - 1];
    if (!callback_val.IsFunction()) {
        Napi::Error::New(info.Env(), "last argument must be a callback function").ThrowAsJavaScriptException();
        return info.Env().Null();
    }

    Napi::Function callback = callback_val.As<Napi::Function>();

    // validate tiles
    if (!info[0].IsArray()) {
        return utils::CallbackError("first arg 'tiles' must be an array of tile objects", info);
    }

    Napi::Array tiles = info[0].As<Napi::Array>();
    unsigned num_tiles = tiles.Length();

    if (num_tiles <= 0) {
        return utils::CallbackError("'tiles' array must be of length greater than 0", info);
    }

    std::unique_ptr<QueryData> query_data = std::make_unique<QueryData>(num_tiles);

    for (unsigned t = 0; t < num_tiles; ++t) {
        Napi::Value tile_val = tiles.Get(t);
        if (!tile_val.IsObject()) {
            return utils::CallbackError("items in 'tiles' array must be objects", info);
        }
        Napi::Object tile_obj = tile_val.As<Napi::Object>();

        // check buffer value
        if (!tile_obj.Has(Napi::String::New(info.Env(), "buffer"))) {
            return utils::CallbackError("item in 'tiles' array does not include a buffer value", info);
        }
        Napi::Value buf_val = tile_obj.Get(Napi::String::New(info.Env(), "buffer"));
        if (buf_val.IsNull() || buf_val.IsUndefined()) {
            return utils::CallbackError("buffer value in 'tiles' array item is null or undefined", info);
        }

        Napi::Object buffer_obj = buf_val.As<Napi::Object>();
        if (!buffer_obj.IsBuffer()) {
            return utils::CallbackError("buffer value in 'tiles' array item is not a true buffer", info);
        }

        Napi::Buffer<char> buffer = buffer_obj.As<Napi::Buffer<char>>();
        // z value
        if (!tile_obj.Has(Napi::String::New(info.Env(), "z"))) {
            return utils::CallbackError("item in 'tiles' array does not include a 'z' value", info);
        }
        Napi::Value z_val = tile_obj.Get(Napi::String::New(info.Env(), "z"));
        if (!z_val.IsNumber()) {
            return utils::CallbackError("'z' value in 'tiles' array item is not an int32", info);
        }

        int z = z_val.As<Napi::Number>().Int32Value();
        if (z < 0) {
            return utils::CallbackError("'z' value must not be less than zero", info);
        }

        // x value
        if (!tile_obj.Has(Napi::String::New(info.Env(), "x"))) {
            return utils::CallbackError("item in 'tiles' array does not include a 'x' value", info);
        }
        Napi::Value x_val = tile_obj.Get(Napi::String::New(info.Env(), "x"));
        if (!x_val.IsNumber()) {
            return utils::CallbackError("'x' value in 'tiles' array item is not an int32", info);
        }

        int x = x_val.As<Napi::Number>().Int32Value();
        if (x < 0) {
            return utils::CallbackError("'x' value must not be less than zero", info);
        }

        // y value
        if (!tile_obj.Has(Napi::String::New(info.Env(), "y"))) {
            return utils::CallbackError("item in 'tiles' array does not include a 'y' value", info);
        }
        Napi::Value y_val = tile_obj.Get(Napi::String::New(info.Env(), "y"));
        if (!y_val.IsNumber()) {
            return utils::CallbackError("'y' value in 'tiles' array item is not an int32", info);
        }

        int y = y_val.As<Napi::Number>().Int32Value();
        if (y < 0) {
            return utils::CallbackError("'y' value must not be less than zero", info);
        }

        // in-place construction
        std::unique_ptr<TileObject> tile{new TileObject{z, x, y, buffer}};
        query_data->tiles.push_back(std::move(tile));
    }

    // validate lng/lat array
    if (!info[1].IsArray()) {
        return utils::CallbackError("second arg 'lnglat' must be an array with [longitude, latitude] values", info);
    }

    Napi::Array lnglat_val = info[1].As<Napi::Array>();
    if (lnglat_val.Length() != 2) {
        return utils::CallbackError("'lnglat' must be an array of [longitude, latitude]", info);
    }

    Napi::Value lng_val = lnglat_val.Get(0u);
    Napi::Value lat_val = lnglat_val.Get(1u);

    if (!lng_val.IsNumber() || !lat_val.IsNumber()) {
        return utils::CallbackError("lnglat values must be numbers", info);
    }
    query_data->longitude = lng_val.ToNumber();
    query_data->latitude = lat_val.ToNumber();
    // validate options object if it exists
    // defaults are set in the QueryData struct.
    if (info.Length() > 3) {

        if (!info[2].IsObject()) {
            return utils::CallbackError("'options' arg must be an object", info);
        }

        Napi::Object options = info[2].ToObject();

        if (options.Has("dedupe")) {
            Napi::Value dedupe_val = options.Get("dedupe");
            if (!dedupe_val.IsBoolean()) {
                return utils::CallbackError("'dedupe' must be a boolean", info);
            }

            bool dedupe = dedupe_val.ToBoolean();
            query_data->dedupe = dedupe;
        }

        if (options.Has("radius")) {
            Napi::Value radius_val = options.Get("radius");
            if (!radius_val.IsNumber()) {
                return utils::CallbackError("'radius' must be a number", info);
            }

            double radius = radius_val.ToNumber();
            if (radius < 0.0) {
                return utils::CallbackError("'radius' must be a positive number", info);
            }

            query_data->radius = radius;
        }

        if (options.Has("limit")) {
            Napi::Value num_results_val = options.Get("limit");
            if (!num_results_val.IsNumber()) {
                return utils::CallbackError("'limit' must be a number", info);
            }
            std::int32_t num_results = num_results_val.As<Napi::Number>().Int32Value();
            if (num_results < 1) {
                return utils::CallbackError("'limit' must be 1 or greater", info);
            }
            if (num_results > 1000) {
                return utils::CallbackError("'limit' must be less than 1000", info);
            }

            query_data->num_results = static_cast<std::uint32_t>(num_results);
        }

        if (options.Has("layers")) {
            Napi::Value layers_val = options.Get("layers");
            if (!layers_val.IsArray()) {
                return utils::CallbackError("'layers' must be an array of strings", info);
            }

            Napi::Array layers_arr = layers_val.As<Napi::Array>();
            unsigned num_layers = layers_arr.Length();

            // only gather layers if there are some in the array
            if (num_layers > 0) {
                for (unsigned j = 0; j < num_layers; ++j) {
                    Napi::Value layer_val = layers_arr.Get(j);
                    if (!layer_val.IsString()) {
                        return utils::CallbackError("'layers' values must be strings", info);
                    }
                    auto layer_utf8_value = std::string(layer_val.ToString());
                    std::size_t layer_str_len = layer_utf8_value.length();
                    if (layer_str_len <= 0) {
                        return utils::CallbackError("'layers' values must be non-empty strings", info);
                    }
                    query_data->layers.emplace_back(layer_utf8_value);
                }
            }
        }

        if (options.Has("geometry")) {
            Napi::Value geometry_val = options.Get("geometry");
            if (!geometry_val.IsString()) {
                return utils::CallbackError("'geometry' option must be a string", info);
            }

            auto geometry_utf8_value = std::string(geometry_val.ToString());
            std::size_t geometry_str_len = geometry_utf8_value.length();
            if (geometry_str_len <= 0) {
                return utils::CallbackError("'geometry' value must be a non-empty string", info);
            }

            if (geometry_utf8_value == "point") {
                query_data->geometry_filter_type = GeomType::point;
            } else if (geometry_utf8_value == "linestring") {
                query_data->geometry_filter_type = GeomType::linestring;
            } else if (geometry_utf8_value == "polygon") {
                query_data->geometry_filter_type = GeomType::polygon;
            } else {
                return utils::CallbackError("'geometry' must be 'point', 'linestring', or 'polygon'", info);
            }
        }
    }

    auto* worker = new Worker{std::move(query_data), callback};
    worker->Queue();
    return info.Env().Undefined();
}

} // namespace VectorTileQuery
