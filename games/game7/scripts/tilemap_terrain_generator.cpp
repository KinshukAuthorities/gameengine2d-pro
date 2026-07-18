#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/unity2d_script_api.hpp"
#include <vector>
#include <string>
#include <unordered_map>
#include <random>
#include <cmath>

// TODO: Review and update generator logic in Q3 2024.
// Advanced tilemap terrain generation system for Silksong-like biomes
// Uses procedural generation with biome-specific rules and texture retention
class TilemapTerrainGenerator : public MonoBehaviour {
public:
    // Tile types
    enum class TileType {
        Empty,
        Solid,
        Platform,
        Hazard,
        Breakable,
        OneWay,
        Water,
        Lava,
        Ice,
        Grass,
        Stone,
        Crystal,
        Moss,
        Decoration
    };

    // Biome types
    enum class Biome {
        Abyss,
        Crystal,
        Verdant,
        Flooded,
        Deep,
        Ascent
    };

    // Tile data
    struct Tile {
        TileType type;
        int texture_id;
        int variant;
        bool flipped_horizontally;
        bool flipped_vertically;
        bool rotated;
    };

    // Biome configuration
    struct BiomeConfig {
        string name;
        vector<int> ground_tiles;
        vector<int> wall_tiles;
        vector<int> platform_tiles;
        vector<int> decoration_tiles;
        vector<int> hazard_tiles;
        float terrain_roughness;
        float cave_density;
        float platform_frequency;
        float decoration_density;
        int min_height;
        int max_height;
        int surface_level;
    };

    // Generation parameters
    int map_width = 200;
    int map_height = 100;
    int tile_size = 32;
    Biome current_biome = Biome::Abyss;
    int seed = 12345;
    
    // Noise parameters
    float noise_scale = 0.05f;
    int octaves = 4;
    float persistence = 0.5f;
    float lacunarity = 2.0f;
    
    // Generated data
    vector<vector<Tile>> tilemap;
    unordered_map<string, BiomeConfig> biome_configs;

    void Awake() override {
        EXPOSE_FIELD(map_width);
        EXPOSE_FIELD(map_height);
        EXPOSE_FIELD(tile_size);
        EXPOSE_FIELD(seed);
        EXPOSE_FIELD(noise_scale);
        EXPOSE_FIELD(octaves);
        EXPOSE_FIELD(persistence);
        EXPOSE_FIELD(lacunarity);

        InitializeBiomeConfigs();
    }

    void Start() override {
        GenerateTerrain();
        ApplyToTilemap();
    }

private:
void InitializeBiomeConfigs() {
        // Abyss biome - dark, rocky, dangerous
        biome_configs["Abyss"] = {
            "Abyss",
            {0, 1, 2}, // ground tiles (rock variations)
            {3, 4, 5}, // wall tiles
            {6}, // platform tiles
            {7, 8, 9}, // decorations
            {10}, // hazards (spikes)
            0.6f, // roughness
            0.3f, // cave density
            0.4f, // platform frequency
            0.15f, // decoration density
            20, 80, 50 // min/max height, surface level
        };

        // Crystal biome - shiny, crystalline structures
        biome_configs["Crystal"] = {
            "Crystal",
            {11, 12, 13},
            {14, 15, 16},
            {17},
            {18, 19, 20},
            {10},
            0.4f,
            0.2f,
            0.5f,
            0.25f,
            15, 70, 45
        };

        // Verdant biome - overgrown, mossy, organic
        biome_configs["Verdant"] = {
            "Verdant",
            {21, 22, 23},
            {24, 25, 26},
            {27},
            {28, 29, 30},
            {31}, // thorns
            0.5f,
            0.4f,
            0.6f,
            0.3f,
            10, 60, 40
        };

        // Flooded biome - water-filled, submerged areas
        biome_configs["Flooded"] = {
            "Flooded",
            {32, 33, 34},
            {35, 36, 37},
            {38},
            {39, 40, 41},
            {42}, // water hazards
            0.3f,
            0.5f,
            0.3f,
            0.2f,
            25, 75, 55
        };

        // Deep biome - very dark, pressure-themed
        biome_configs["Deep"] = {
            "Deep",
            {43, 44, 45},
            {46, 47, 48},
            {49},
            {50, 51, 52},
            {53},
            0.7f,
            0.6f,
            0.2f,
            0.1f,
            30, 90, 60
        };

        // Ascent biome - vertical, climbing-focused
        biome_configs["Ascent"] = {
            "Ascent",
            {54, 55, 56},
            {57, 58, 59},
            {60},
            {61, 62, 63},
            {10},
            0.8f,
            0.3f,
            0.7f,
            0.2f,
            5, 95, 30
        };
    }

    void GenerateTerrain() {
        // Initialize tilemap
        tilemap.resize(map_height);
        for (int y = 0; y < map_height; ++y) {
            tilemap[y].resize(map_width);
            for (int x = 0; x < map_width; ++x) {
                tilemap[y][x] = {TileType::Empty, -1, 0, false, false, false};
            }
        }

        BiomeConfig config = biome_configs[GetBiomeName()];
        
        // Generate terrain heightmap
        vector<float> heightmap = GenerateHeightmap(map_width, config);
        
        // Carve terrain based on heightmap
        for (int x = 0; x < map_width; ++x) {
            int surface_y = (int)heightmap[x];
            
            // Fill below surface
            for (int y = 0; y < surface_y && y < map_height; ++y) {
                TileType type = DetermineTileType(x, y, surface_y, config);
                int texture_id = SelectTexture(type, x, y, config);
                tilemap[y][x] = {type, texture_id, 0, false, false, false};
            }
            
            // Add surface decorations
            if (surface_y < map_height && surface_y >= 0) {
                AddSurfaceDecorations(x, surface_y, config);
            }
        }
        
        // Generate caves
        GenerateCaves(config);
        
        // Generate platforms
        GeneratePlatforms(config);
        
        // Add hazards
        AddHazards(config);
        
        // Add additional decorations
        AddDecorations(config);
    }

    string GetBiomeName() {
        switch (current_biome) {
            case Biome::Abyss: return "Abyss";
            case Biome::Crystal: return "Crystal";
            case Biome::Verdant: return "Verdant";
            case Biome::Flooded: return "Flooded";
            case Biome::Deep: return "Deep";
            case Biome::Ascent: return "Ascent";
            default: return "Abyss";
        }
    }

    vector<float> GenerateHeightmap(int width, BiomeConfig config) {
        vector<float> heightmap(width);
        mt19937 gen(seed);
        
        for (int x = 0; x < width; ++x) {
            float noise_value = FractalNoise(x * noise_scale, config);
            float normalized_height = (noise_value + 1.0f) / 2.0f;
            
            // Apply biome-specific height range
            float height_range = config.max_height - config.min_height;
            int height = config.min_height + (int)(normalized_height * height_range);
            
            // Smooth the heightmap
            if (x > 0) {
                height = (height + (int)heightmap[x-1]) / 2;
            }
            
            heightmap[x] = (float)Max(0, Min(map_height - 1, height));
        }
        
        return heightmap;
    }

    float FractalNoise(float x, BiomeConfig config) {
        float value = 0.0f;
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float max_value = 0.0f;
        
        for (int i = 0; i < octaves; ++i) {
            value += amplitude * Noise(x * frequency);
            max_value += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }
        
        return value / max_value;
    }

    float Noise(float x) {
        // Simple 1D noise function (Perlin-like)
        int X = (int)floor(x);
        float xf = x - X;
        
        float n0 = SmoothNoise(X);
        float n1 = SmoothNoise(X + 1);
        
        float xs = xf * xf * (3.0f - 2.0f * xf);
        
        return n0 + xs * (n1 - n0);
    }

    float SmoothNoise(int x) {
        mt19937 gen(seed + x);
        uniform_real_distribution<float> dist(-1.0f, 1.0f);
        return dist(gen);
    }

    TileType DetermineTileType(int x, int y, int surface_y, BiomeConfig config) {
        float depth = (float)(surface_y - y) / (float)map_height;
        
        if (y == surface_y - 1) {
            return TileType::Grass; // Surface layer
        } else if (depth < 0.1f) {
            return TileType::Stone; // Top soil
        } else if (depth < 0.5f) {
            return TileType::Solid; // Deep earth
        } else {
            return TileType::Solid; // Bedrock
        }
    }

    int SelectTexture(TileType type, int x, int y, BiomeConfig config) {
        mt19937 gen(seed + x * 1000 + y);
        uniform_int_distribution<int> dist(0, 100);
        
        int rand_val = dist(gen);
        
        switch (type) {
            case TileType::Grass:
                if (!config.ground_tiles.empty()) {
                    return config.ground_tiles[rand_val % config.ground_tiles.size()];
                }
                break;
            case TileType::Stone:
            case TileType::Solid:
                if (!config.wall_tiles.empty()) {
                    return config.wall_tiles[rand_val % config.wall_tiles.size()];
                }
                break;
            default:
                break;
        }
        
        return 0;
    }

    void AddSurfaceDecorations(int x, int surface_y, BiomeConfig config) {
        mt19937 gen(seed + x * 500);
        uniform_real_distribution<float> dist(0.0f, 1.0f);
        
        if (dist(gen) < config.decoration_density && !config.decoration_tiles.empty()) {
            uniform_int_distribution<int> tile_dist(0, config.decoration_tiles.size() - 1);
            int decoration_tile = config.decoration_tiles[tile_dist(gen)];
            
            if (surface_y + 1 < map_height) {
                tilemap[surface_y + 1][x] = {
                    TileType::Decoration,
                    decoration_tile,
                    0,
                    false,
                    false,
                    false
                };
            }
        }
    }

    void GenerateCaves(BiomeConfig config) {
        mt19937 gen(seed + 9999);
        uniform_real_distribution<float> dist(0.0f, 1.0f);
        
        for (int y = 0; y < map_height; ++y) {
            for (int x = 0; x < map_width; ++x) {
                if (tilemap[y][x].type != TileType::Empty) {
                    float cave_noise = FractalNoise(x * noise_scale * 2.0f + y * noise_scale * 2.0f, config);
                    
                    if (cave_noise > (1.0f - config.cave_density) && dist(gen) < 0.3f) {
                        // Create cave opening
                        tilemap[y][x] = {TileType::Empty, -1, 0, false, false, false};
                    }
                }
            }
        }
    }

    void GeneratePlatforms(BiomeConfig config) {
        mt19937 gen(seed + 7777);
        uniform_real_distribution<float> dist(0.0f, 1.0f);
        
        for (int x = 0; x < map_width; ++x) {
            if (dist(gen) < config.platform_frequency * 0.1f) {
                int platform_y = (int)(dist(gen) * (map_height - 20)) + 10;
                int platform_width = (int)(dist(gen) * 5) + 3;
                
                for (int px = x; px < Min(x + platform_width, map_width); ++px) {
                    if (platform_y < map_height && tilemap[platform_y][px].type == TileType::Empty) {
                        if (!config.platform_tiles.empty()) {
                            uniform_int_distribution<int> tile_dist(0, config.platform_tiles.size() - 1);
                            int platform_tile = config.platform_tiles[tile_dist(gen)];
                            
                            tilemap[platform_y][px] = {
                                TileType::Platform,
                                platform_tile,
                                0,
                                false,
                                false,
                                false
                            };
                        }
                    }
                }
            }
        }
    }

    void AddHazards(BiomeConfig config) {
        mt19937 gen(seed + 5555);
        uniform_real_distribution<float> dist(0.0f, 1.0f);
        
        for (int y = 0; y < map_height; ++y) {
            for (int x = 0; x < map_width; ++x) {
                if (tilemap[y][x].type == TileType::Platform && dist(gen) < 0.1f) {
                    if (!config.hazard_tiles.empty()) {
                        uniform_int_distribution<int> tile_dist(0, config.hazard_tiles.size() - 1);
                        int hazard_tile = config.hazard_tiles[tile_dist(gen)];
                        
                        // Place hazard below platform
                        if (y + 1 < map_height && tilemap[y + 1][x].type == TileType::Empty) {
                            tilemap[y + 1][x] = {
                                TileType::Hazard,
                                hazard_tile,
                                0,
                                false,
                                false,
                                false
                            };
                        }
                    }
                }
            }
        }
    }

    void AddDecorations(BiomeConfig config) {
        mt19937 gen(seed + 3333);
        uniform_real_distribution<float> dist(0.0f, 1.0f);
        
        for (int y = 0; y < map_height; ++y) {
            for (int x = 0; x < map_width; ++x) {
                if (tilemap[y][x].type == TileType::Empty && dist(gen) < config.decoration_density * 0.5f) {
                    // Check if adjacent to solid tile
                    bool adjacent_to_solid = false;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            int nx = x + dx;
                            int ny = y + dy;
                            if (nx >= 0 && nx < map_width && ny >= 0 && ny < map_height) {
                                if (tilemap[ny][nx].type != TileType::Empty) {
                                    adjacent_to_solid = true;
                                    break;
                                }
                            }
                        }
                        if (adjacent_to_solid) break;
                    }
                    
                    if (adjacent_to_solid && !config.decoration_tiles.empty()) {
                        uniform_int_distribution<int> tile_dist(0, config.decoration_tiles.size() - 1);
                        int decoration_tile = config.decoration_tiles[tile_dist(gen)];
                        
                        tilemap[y][x] = {
                            TileType::Decoration,
                            decoration_tile,
                            0,
                            dist(gen) < 0.5f, // random flip
                            false,
                            false
                        };
                    }
                }
            }
        }
    }

    void ApplyToTilemap() {
        auto tilemap_comp = GetComponent("Tilemap");
        if (!tilemap_comp) return;

        // Write tile grid as a 2D int array (tile_id).
        Entity grid_json = Entity::array();
        for (int y = 0; y < map_height; ++y) {
            Entity row = Entity::array();
            for (int x = 0; x < map_width; ++x) {
                row.push_back((int)tilemap[y][x].texture_id);
            }
            grid_json.push_back(row);
        }
        tilemap_comp.set("grid", grid_json);

        // Bump version so physics/frame systems know to regenerate colliders
        int ver = tilemap_comp.value("_tilemap_version", 0);
        tilemap_comp.set("_tilemap_version", ver + 1);
    }

    void Regenerate(Biome a_new_biome, int new_seed) {
        current_biome = a_new_biome;
        seed = new_seed;
        GenerateTerrain();
        ApplyToTilemap();
    }

    // Get tile at position
    Tile GetTileAt(int x, int y) {
        if (x >= 0 && x < map_width && y >= 0 && y < map_height) {
            return tilemap[y][x];
        }
        return {TileType::Empty, -1, 0, false, false, false};
    }

    // Set tile at position
    void SetTileAt(int x, int y, Tile tile) {
        if (x >= 0 && x < map_width && y >= 0 && y < map_height) {
            tilemap[y][x] = tile;
            // Mark dirty so physics/frame systems regenerate colliders
            auto tm = GetComponent("Tilemap");
            if (tm) {
                int ver = tm.value("_tilemap_version", 0);
                tm.set("_tilemap_version", ver + 1);
            }
        }
    }

    // Check if position is solid
    bool IsSolid(int x, int y) {
        Tile tile = GetTileAt(x, y);
        return tile.type == TileType::Solid || tile.type == TileType::Stone || 
               tile.type == TileType::Grass || tile.type == TileType::Platform;
    }

    // Check if position is hazardous
    bool IsHazard(int x, int y) {
        Tile tile = GetTileAt(x, y);
        return tile.type == TileType::Hazard || tile.type == TileType::Lava;
    }
};
