// Copyright 2014 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "precompiled.h"
#include "angle.h"
#include "audio_config_generated.h"
#include "audio_engine.h"
#include "character_state_machine.h"
#include "character_state_machine_def_generated.h"
#include "config_generated.h"
#include "impel_flatbuffers.h"
#include "impel_processor_overshoot.h"
#include "impel_processor_smooth.h"
#include "pie_noon_common_generated.h"
#include "pie_noon_game.h"
#include "timeline_generated.h"
#include "touchscreen_controller.h"
#include "utilities.h"
#include "SDL_timer.h"

using mathfu::vec2i;
using mathfu::vec2;
using mathfu::vec3;
using mathfu::vec4;
using mathfu::mat3;
using mathfu::mat4;

namespace fpl {
namespace pie_noon {

static const int kQuadNumVertices = 4;
static const int kQuadNumIndices = 6;

static const unsigned short kQuadIndices[] = { 0, 1, 2, 2, 1, 3 };

static const Attribute kQuadMeshFormat[] =
    { kPosition3f, kTexCoord2f, kNormal3f, kTangent4f, kEND };

static const char kAssetsDir[] = "assets";

static const char kConfigFileName[] = "config.bin";

#ifdef __ANDROID__
static const int kAndroidMaxScreenWidth = 1920;
static const int kAndroidMaxScreenHeight = 1080;
#endif

// Return the elapsed milliseconds since the start of the program. This number
// will loop back to 0 after about 49 days; always take the difference to
// properly handle the wrap-around case.
static inline WorldTime CurrentWorldTime() {
  return SDL_GetTicks();
}

static inline const UiGroup* TitleScreenButtons(const Config& config) {
#ifdef __ANDROID__
  return config.title_screen_buttons_android();
#else
  return config.title_screen_buttons_non_android();
#endif
}

/// kVersion is used by Google developers to identify which
/// applications uploaded to Google Play are derived from this application.
/// This allows the development team at Google to determine the popularity of
/// this application.
/// How it works: Applications that are uploaded to the Google Play Store are
/// scanned for this version string.  We track which applications are using it
/// to measure popularity.  You are free to remove it (of course) but we would
/// appreciate if you left it in.
static const char kVersion[] = "Pie Noon 1.0.0";

PieNoonGame::PieNoonGame()
    : state_(kUninitialized),
      state_entry_time_(0),
      matman_(renderer_),
      cardboard_fronts_(RenderableId_Count, nullptr),
      cardboard_backs_(RenderableId_Count, nullptr),
      stick_front_(nullptr),
      stick_back_(nullptr),
      shader_lit_textured_normal_(nullptr),
      shader_simple_shadow_(nullptr),
      shader_textured_(nullptr),
      shader_grayscale_(nullptr),
      shadow_mat_(nullptr),
      prev_world_time_(0),
      debug_previous_states_(),
      full_screen_fader_(&renderer_),
      fade_exit_state_(kUninitialized),
      ambience_channel_(AudioEngine::kInvalidChannel),
      stinger_channel_(AudioEngine::kInvalidChannel),
      next_achievement_index_(0) {
  version_ = kVersion;
}

PieNoonGame::~PieNoonGame() {
  for (int i = 0; i < RenderableId_Count; ++i) {
    delete cardboard_fronts_[i];
    cardboard_fronts_[i] = nullptr;

    delete cardboard_backs_[i];
    cardboard_backs_[i] = nullptr;
  }

  delete stick_front_;
  stick_front_ = nullptr;

  delete stick_back_;
  stick_back_ = nullptr;
}

bool PieNoonGame::InitializeConfig() {
  if (!LoadFile(kConfigFileName, &config_source_)) {
    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "can't load config.bin\n");
    return false;
  }
  return true;
}

// Initialize the 'renderer_' member. No other members have been initialized at
// this point.
bool PieNoonGame::InitializeRenderer() {
  const Config& config = GetConfig();

#ifdef __ANDROID__
  auto max_screen_size = pie_noon::Vec2i(kAndroidMaxScreenWidth, kAndroidMaxScreenHeight);
  auto window_size = &max_screen_size;
#else
  auto window_size = config.window_size();
#endif
  assert(window_size);
  if (!renderer_.Initialize(LoadVec2i(window_size),
                            config.window_title()->c_str())) {
    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Renderer initialization error: %s\n",
            renderer_.last_error().c_str());
    return false;
  }

  renderer_.color() = mathfu::kOnes4f;
  // Initialize the first frame as black.
  renderer_.ClearFrameBuffer(mathfu::kZeros4f);
  return true;
}

// Initializes 'vertices' at the specified position, aligned up-and-down.
// 'vertices' must be an array of length kQuadNumVertices.
static void CreateVerticalQuad(const vec3& offset, const vec2& geo_size,
                               const vec2& texture_coord_size,
                               NormalMappedVertex* vertices) {
  const float half_width = geo_size[0] * 0.5f;
  const vec3 bottom_left = offset + vec3(-half_width, 0.0f, 0.0f);
  const vec3 top_right = offset + vec3(half_width, geo_size[1], 0.0f);

  vertices[0].pos = bottom_left;
  vertices[1].pos = vec3(top_right[0], bottom_left[1], offset[2]);
  vertices[2].pos = vec3(bottom_left[0], top_right[1], offset[2]);
  vertices[3].pos = top_right;

  const float coord_half_width = texture_coord_size[0] * 0.5f;
  const vec2 coord_bottom_left(0.5f - coord_half_width, 1.0f);
  const vec2 coord_top_right(0.5f + coord_half_width,
                             1.0f - texture_coord_size[1]);

  vertices[0].tc = coord_bottom_left;
  vertices[1].tc = vec2(coord_top_right[0], coord_bottom_left[1]);
  vertices[2].tc = vec2(coord_bottom_left[0], coord_top_right[1]);
  vertices[3].tc = coord_top_right;

  Mesh::ComputeNormalsTangents(vertices, &kQuadIndices[0], kQuadNumVertices,
                               kQuadNumIndices);
}

// Creates a mesh of a single quad (two triangles) vertically upright.
// The quad's has x and y size determined by the size of the texture.
// The quad is offset in (x,y,z) space by the 'offset' variable.
// Returns a mesh with the quad and texture, or nullptr if anything went wrong.
Mesh* PieNoonGame::CreateVerticalQuadMesh(
    const flatbuffers::String* material_name, const vec3& offset,
    const vec2& pixel_bounds, float pixel_to_world_scale) {

  // Don't try to load obviously invalid materials. Suppresses error logs from
  // the material manager.
  if (material_name == nullptr || material_name->c_str()[0] == '\0')
    return nullptr;

  // Load the material from file, and check validity.
  Material* material = matman_.LoadMaterial(material_name->c_str());
  bool material_valid = material != nullptr && material->textures().size() > 0;
  if (!material_valid)
    return nullptr;

  // Create vertex geometry in proportion to the texture size.
  // This is nice for the artist since everything is at the scale of the
  // original artwork.
  assert(pixel_bounds.x() && pixel_bounds.y());
  const vec2 texture_size = vec2(mathfu::RoundUpToPowerOf2(pixel_bounds.x()),
                                 mathfu::RoundUpToPowerOf2(pixel_bounds.y()));
  const vec2 texture_coord_size = pixel_bounds / texture_size;
  const vec2 geo_size = pixel_bounds * vec2(pixel_to_world_scale);

  // Initialize a vertex array in the requested position.
  NormalMappedVertex vertices[kQuadNumVertices];
  CreateVerticalQuad(offset, geo_size, texture_coord_size, vertices);

  // Create mesh and add in quad indices.
  Mesh* mesh = new Mesh(vertices, kQuadNumVertices, sizeof(NormalMappedVertex),
                        kQuadMeshFormat);
  mesh->AddIndices(kQuadIndices, kQuadNumIndices, material);
  return mesh;
}

// Load textures for cardboard into 'materials_'. The 'renderer_' and 'matman_'
// members have been initialized at this point.
bool PieNoonGame::InitializeRenderingAssets() {
  const Config& config = GetConfig();

  // Check data validity.
  if (config.renderables()->Length() != RenderableId_Count) {
    SDL_LogError(SDL_LOG_CATEGORY_ERROR,
                 "%s's 'renderables' array has %d entries, needs %d.\n",
                 kConfigFileName, config.renderables()->Length(),
                 RenderableId_Count);
    return false;
  }

  // Force these textures to be queued up first, since we want to use them for
  // the loading screen.
  matman_.LoadMaterial(config.loading_material()->c_str());
  matman_.LoadMaterial(config.loading_logo()->c_str());
  matman_.LoadMaterial(config.fade_material()->c_str());

  // Create a mesh for the front and back of each cardboard cutout.
  const vec3 front_z_offset(0.0f, 0.0f, config.cardboard_front_z_offset());
  const vec3 back_z_offset(0.0f, 0.0f, config.cardboard_back_z_offset());
  for (int id = 0; id < RenderableId_Count; ++id) {
    auto renderable = config.renderables()->Get(id);
    const vec3 offset = renderable->offset() == nullptr ? mathfu::kZeros3f :
                        LoadVec3(renderable->offset());
    const vec3 front_offset = offset + front_z_offset;
    const vec3 back_offset = offset + back_z_offset;
    const auto pixel_bounds_ptr = renderable->pixel_bounds();
    const vec2 pixel_bounds(pixel_bounds_ptr == nullptr ? mathfu::kZeros2i :
                            LoadVec2i(pixel_bounds_ptr));
    const float pixel_to_world_scale = renderable->geometry_scale() *
                                       config.pixel_to_world_scale();

    cardboard_fronts_[id] = CreateVerticalQuadMesh(
        renderable->cardboard_front(), front_offset, pixel_bounds,
        pixel_to_world_scale);

    cardboard_backs_[id] = CreateVerticalQuadMesh(
        renderable->cardboard_back(), back_offset, pixel_bounds,
        pixel_to_world_scale);
  }

  // We default to the invalid texture, so it has to exist.
  if (!cardboard_fronts_[RenderableId_Invalid]) {
    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Can't load backup texture.\n");
    return false;
  }

  // Create stick front and back meshes.
  const vec3 stick_front_offset(0.0f, config.stick_y_offset(),
                                config.stick_front_z_offset());
  const vec3 stick_back_offset(0.0f, config.stick_y_offset(),
                               config.stick_back_z_offset());
  stick_front_ = CreateVerticalQuadMesh(config.stick_front(),
                                        stick_front_offset,
                                        LoadVec2(config.stick_bounds()),
                                        config.pixel_to_world_scale());
  stick_back_ = CreateVerticalQuadMesh(config.stick_back(), stick_back_offset,
                                       LoadVec2(config.stick_bounds()),
                                       config.pixel_to_world_scale());

  // Load all shaders we use:
  shader_lit_textured_normal_ =
      matman_.LoadShader("shaders/lit_textured_normal");
  shader_cardboard = matman_.LoadShader("shaders/cardboard");
  shader_simple_shadow_ = matman_.LoadShader("shaders/simple_shadow");
  shader_textured_ = matman_.LoadShader("shaders/textured");
  shader_grayscale_ = matman_.LoadShader("shaders/grayscale");
  if (!(shader_lit_textured_normal_ &&
        shader_cardboard &&
        shader_simple_shadow_ &&
        shader_textured_ &&
        shader_grayscale_)) return false;

  // Load shadow material:
  shadow_mat_ = matman_.LoadMaterial("materials/floor_shadows.bin");
  if (!shadow_mat_) return false;

  // Load all the menu textures.
  gui_menu_.LoadAssets(TitleScreenButtons(config), &matman_);
  gui_menu_.LoadAssets(config.touchscreen_zones(), &matman_);
  gui_menu_.LoadAssets(config.pause_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.join_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.extras_screen_buttons(), &matman_);

  // Configure the full screen fader.
  full_screen_fader_.set_material(matman_.FindMaterial(
      config.fade_material()->c_str()));
  full_screen_fader_.set_shader(shader_textured_);

  // Start the thread that actually loads all assets we requested above.
  matman_.StartLoadingTextures();

  return true;
}

// Create state matchines, characters, controllers, etc. present in
// 'gamestate_'.
bool PieNoonGame::InitializeGameState() {
  const Config& config = GetConfig();

  game_state_.set_config(&config);

  // Register the impeller types with the ImpelEngine.
  impel::OvershootImpelProcessor::Register();
  impel::SmoothImpelProcessor::Register();

  // Load flatbuffer into buffer.
  if (!LoadFile("character_state_machine_def.bin", &state_machine_source_)) {
    SDL_LogError(SDL_LOG_CATEGORY_ERROR,
                 "Error loading character state machine.\n");
    return false;
  }

  // Grab the state machine from the buffer.
  auto state_machine_def = GetStateMachine();
  if (!CharacterStateMachineDef_Validate(state_machine_def)) {
    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "State machine is invalid.\n");
    return false;
  }

  for (int i = 0; i < ControlScheme::kDefinedControlSchemeCount; i++) {
    PlayerController* controller = new PlayerController();
    controller->Initialize(&input_, ControlScheme::GetDefaultControlScheme(i));
    AddController(controller);
  }

  // Add a touch screen controller into the controller list, so that touch
  // inputs are processed correctly and assigned a character:
  touch_controller_ = new TouchscreenController();

  vec2 window_size = vec2(static_cast<float>(renderer_.window_size().x()),
                          static_cast<float>(renderer_.window_size().y()));
  touch_controller_->Initialize(&input_, window_size, &config);

  AddController(touch_controller_);

  // Create characters.
  for (unsigned int i = 0; i < config.character_count(); ++i) {
    AiController* controller = new AiController();
    controller->Initialize(&game_state_, &config, i);
    game_state_.characters().push_back(std::unique_ptr<Character>(
        new Character(i, controller, config, state_machine_def,
                      &audio_engine_)));
    AddController(controller);
    controller->Initialize(&game_state_, &config, i);
  }

  debug_previous_states_.resize(config.character_count(), -1);

  return true;
}

class AudioEngineVolumeControl {
 public:
  AudioEngineVolumeControl(AudioEngine* audio) : audio_(audio) {}
  void operator()(SDL_Event* event) {
    switch (event->type) {
      case SDL_APP_WILLENTERBACKGROUND:
        audio_->Pause(true);
        break;
      case SDL_APP_DIDENTERFOREGROUND:
        audio_->Pause(false);
        break;
      default:
        break;
    }
  }
 private:
  AudioEngine* audio_;
};

// Initialize each member in turn. This is logically just one function, since
// the order of initialization cannot be changed. However, it's nice for
// debugging and readability to have each section lexographically separate.
bool PieNoonGame::Initialize(const char* const binary_directory) {
  SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PieNoon initializing...\n");

  if (!ChangeToUpstreamDir(binary_directory, kAssetsDir))
    return false;

  if (!InitializeConfig())
    return false;

  if (!InitializeRenderer())
    return false;

  if (!InitializeRenderingAssets())
    return false;

  input_.Initialize();

  // Some people are having trouble loading the audio engine, and it's not
  // strictly necessary for gameplay, so don't die if the audio engine fails to
  // initialize.
  audio_engine_.Initialize(GetConfig().audio());
  input_.AddAppEventCallback(AudioEngineVolumeControl(&audio_engine_));

  if (!InitializeGameState())
    return false;

# ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  if (!gpg_manager.Initialize(ReadPreference("logged_in", 1, 1) != 0))
    return false;
# endif

  SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PieNoon initialization complete\n");
  return true;
}

// Returns the mesh for renderable_id, if we have one, or the pajama mesh
// (a mesh with a texture that's obviously wrong), if we don't.
Mesh* PieNoonGame::GetCardboardFront(int renderable_id) {
  const bool is_valid_id = 0 <= renderable_id &&
                           renderable_id < RenderableId_Count &&
                           cardboard_fronts_[renderable_id] != nullptr;
  return is_valid_id ? cardboard_fronts_[renderable_id]
                     : cardboard_fronts_[RenderableId_Invalid];
}

void PieNoonGame::RenderCardboard(const SceneDescription& scene,
                                const mat4& camera_transform) {
  const Config& config = GetConfig();

  for (size_t i = 0; i < scene.renderables().size(); ++i) {
    const auto& renderable = scene.renderables()[i];
    const int id = renderable->id();

    // Set up vertex transformation into projection space.
    const mat4 mvp = camera_transform * renderable->world_matrix();
    renderer_.model_view_projection() = mvp;

    // Set the camera and light positions in object space.
    const mat4 world_matrix_inverse = renderable->world_matrix().Inverse();
    renderer_.camera_pos() = world_matrix_inverse *
                             game_state_.camera().Position();

    // TODO: check amount of lights.
    renderer_.light_pos() = world_matrix_inverse * (*scene.lights()[0]);

    // Note: Draw order is back-to-front, so draw the cardboard back, then
    // popsicle stick, then cardboard front--in that order.
    //
    // If we have a back, draw the back too, slightly offset.
    // The back is the *inside* of the cardboard, representing corrugation.
    if (cardboard_backs_[id]) {
      shader_cardboard->Set(renderer_);
      cardboard_backs_[id]->Render(renderer_);
    }

    // Draw the popsicle stick that props up the cardboard.
    if (config.renderables()->Get(id)->stick() && stick_front_ != nullptr &&
        stick_back_ != nullptr) {
      shader_textured_->Set(renderer_);
      stick_front_->Render(renderer_);
      stick_back_->Render(renderer_);
    }

    renderer_.color() = renderable->color();

    if (config.renderables()->Get(id)->cardboard()) {
      shader_cardboard->Set(renderer_);
      shader_cardboard->SetUniform("ambient_material",
          LoadVec3(config.cardboard_ambient_material()));
      shader_cardboard->SetUniform("diffuse_material",
          LoadVec3(config.cardboard_diffuse_material()));
      shader_cardboard->SetUniform("specular_material",
          LoadVec3(config.cardboard_specular_material()));
      shader_cardboard->SetUniform("shininess",
          config.cardboard_shininess());
      shader_cardboard->SetUniform("normalmap_scale",
          config.cardboard_normalmap_scale());
    } else {
      shader_textured_->Set(renderer_);
    }
    Mesh* front = GetCardboardFront(id);
    front->Render(renderer_);
  }
}

void PieNoonGame::Render(const SceneDescription& scene) {
  const Config& config = GetConfig();

  // Final matrix that applies the view frustum to bring into screen space.
  auto res = renderer_.window_size();
  mat4 perspective_matrix_ = mat4::Perspective(
      config.viewport_angle(), res.x() / static_cast<float>(res.y()),
      config.viewport_near_plane(), config.viewport_far_plane(), -1.0f);
  const mat4 camera_transform = perspective_matrix_ * scene.camera();

  // Render a ground plane.
  // TODO: Replace with a regular environment prop. Calculate scale_bias from
  // environment prop size.
  renderer_.model_view_projection() = camera_transform;
  renderer_.color() = mathfu::kOnes4f;
  shader_textured_->Set(renderer_);
  auto ground_mat = matman_.LoadMaterial("materials/floor.bin");
  assert(ground_mat);
  ground_mat->Set(renderer_);
  const float ground_width = 16.4f;
  const float ground_depth = 8.0f;
  Mesh::RenderAAQuadAlongX(vec3(-ground_width, 0, 0),
                           vec3(ground_width, 0, ground_depth),
                           vec2(0, 0), vec2(1.0f, 1.0f));
  const vec4 world_scale_bias(1.0f / (2.0f * ground_width), 1.0f / ground_depth,
                              0.5f, 0.0f);

  // Render shadows for all Renderables first, with depth testing off so
  // they blend properly.
  renderer_.DepthTest(false);
  renderer_.model_view_projection() = camera_transform;
  renderer_.light_pos() = *scene.lights()[0];  // TODO: check amount of lights.
  shader_simple_shadow_->SetUniform("world_scale_bias", world_scale_bias);
  for (size_t i = 0; i < scene.renderables().size(); ++i) {
    const auto& renderable = scene.renderables()[i];
    const int id = renderable->id();
    Mesh* front = GetCardboardFront(id);
    if (config.renderables()->Get(id)->shadow()) {
      renderer_.model() = renderable->world_matrix();
      shader_simple_shadow_->Set(renderer_);
      // The first texture of the shadow shader has to be that of the
      // billboard.
      shadow_mat_->textures()[0] = front->GetMaterial(0)->textures()[0];
      shadow_mat_->Set(renderer_);
      front->Render(renderer_, true);
    }
  }
  renderer_.DepthTest(true);

  // Now render the Renderables normally, on top of the shadows.
  RenderCardboard(scene, camera_transform);
}

void PieNoonGame::Render2DElements() {
  // Set up an ortho camera for all 2D elements, with (0, 0) in the top left,
  // and the bottom right the windows size in pixels.
  auto res = renderer_.window_size();
  auto ortho_mat = mathfu::OrthoHelper<float>(
      0.0f, static_cast<float>(res.x()), static_cast<float>(res.y()), 0.0f,
      -1.0f, 1.0f);
  renderer_.model_view_projection() = ortho_mat;

  // Update the currently drawing Google Play Games image. Displays "Sign In"
  // when currently signed-out, and "Sign Out" when currently signed in.
# ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  bool is_logged_in = gpg_manager.LoggedIn();
  const int material_index = is_logged_in ? 0 : 1;

  auto gpg_button = gui_menu_.FindButtonById(ButtonId_MenuSignIn);
  if (gpg_button)
    gpg_button->set_current_up_material(material_index);

  auto gpg_text = gui_menu_.FindImageById(ButtonId_MenuSignInText);
  if (gpg_text)
    gpg_text->set_current_material_index(material_index);

  auto achievements_button = gui_menu_.FindButtonById(ButtonId_MenuAchievements);
  if (achievements_button)
    achievements_button->set_is_active(is_logged_in);
  auto leaderboards_button = gui_menu_.FindButtonById(ButtonId_MenuLeaderboard);
  if (leaderboards_button)
    leaderboards_button->set_is_active(is_logged_in);

# endif

  // Loop through the 2D elements. Draw each subsequent one slightly closer
  // to the camera so that they appear on top of the previous ones.
  gui_menu_.Render(&renderer_);
}


// Debug function to print out state machine transitions.
void PieNoonGame::DebugPrintCharacterStates() {
  // Display the state changes, at least until we get real rendering up.
  for (size_t i = 0; i < game_state_.characters().size(); ++i) {
    auto& character = game_state_.characters()[i];
    auto id = character->state_machine()->current_state()->id();
    if (debug_previous_states_[i] != id) {
      SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                  "character %d - Health %2d, State %s [%d]\n",
                  i, character->health(), EnumNameStateId(id), id);
      debug_previous_states_[i] = id;
    }
  }
}

// Debug function to print out the state of each AirbornePie.
void PieNoonGame::DebugPrintPieStates() {
  for (unsigned int i = 0; i < game_state_.pies().size(); ++i) {
    auto& pie = game_state_.pies()[i];
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Pie from [%i]->[%i] w/ %i dmg at pos[%.2f, %.2f, %.2f]\n",
                pie->source(), pie->target(), pie->damage(),
                pie->position().x(), pie->position().y(), pie->position().z());
  }
}

const Config& PieNoonGame::GetConfig() const {
  return *fpl::pie_noon::GetConfig(config_source_.c_str());
}

const CharacterStateMachineDef* PieNoonGame::GetStateMachine() const {
  return fpl::pie_noon::GetCharacterStateMachineDef(
    state_machine_source_.c_str());
}

struct ButtonToTranslation {
  int button;
  vec3 translation;
};

// Debug function to move the camera if the mouse button is down.
void PieNoonGame::DebugCamera() {
  const Config& config = GetConfig();

  // Only move the camera if the left mouse button (or first finger) is down.
  if (!input_.GetButton(SDLK_POINTER1).is_down())
    return;

  static const ButtonToTranslation kDebugCameraButtons[] = {
    { 'd', mathfu::kAxisX3f },
    { 'a', -mathfu::kAxisX3f },
    { 'w', mathfu::kAxisZ3f },
    { 's', -mathfu::kAxisZ3f },
    { 'q', mathfu::kAxisY3f },
    { 'e', -mathfu::kAxisY3f },
  };

  // Convert key presses to translations along camera axes.
  vec3 camera_translation(mathfu::kZeros3f);
  for (size_t i = 0; i < ARRAYSIZE(kDebugCameraButtons); ++i) {
    const ButtonToTranslation& button = kDebugCameraButtons[i];
    if (input_.GetButton(button.button).is_down()) {
      camera_translation += button.translation;
    }
  }

  // Camera rotation is a function of how much the mouse is moved (or finger
  // is dragged).
  const vec2 mouse_delta = vec2(input_.pointers_[0].mousedelta);

  // Return early if there is no change on the camera.
  const bool translate = camera_translation[0] != 0.0f ||
                         camera_translation[1] != 0.0f ||
                         camera_translation[2] != 0.0f;
  const bool rotate = mouse_delta[0] != 0.0f || mouse_delta[1] != 0.0f;
  if (!translate && !rotate)
    return;

  // Calculate the ortho-normal axes of camera space.
  GameCamera& camera = game_state_.camera();
  const vec3 forward = camera.Forward();
  const vec3 side = camera.Side();
  const vec3 up = camera.Up();

  // Convert translation from camera space to world space and scale.
  if (translate) {
    const vec3 scale = LoadVec3(config.button_to_camera_translation_scale());
    const vec3 world_translation = scale * (camera_translation[0] * side +
                                            camera_translation[1] * up +
                                            camera_translation[2] * forward);
    const vec3 new_position = camera.Position() + world_translation;
    camera.OverridePosition(new_position);

    if (config.print_camera_orientation()) {
      SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                   "camera position (%.5ff, %.5ff, %.5ff)\n",
                   new_position[0], new_position[1], new_position[2]);
    }
  }

  // Move the camera target in the camera plane.
  if (rotate) {
    // Apply mouse movement along up and side axes. Scale so that no matter
    // distance, the same angle is applied.
    const float dist = camera.Dist();
    const float scale = dist * config.mouse_to_camera_rotation_scale();
    const vec3 unscaled_delta = mouse_delta.x() * side + mouse_delta.y() * up;
    const vec3 target_delta = scale * unscaled_delta;
    const vec3 new_target = camera.Target() + target_delta;
    camera.OverrideTarget(new_target);

    if (config.print_camera_orientation()) {
      SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                  "camera target (%.5ff, %.5ff, %.5ff)\n",
                  new_target[0], new_target[1], new_target[2]);
    }
  }
}

// The join menu has a series of images that disappear one-by-one.
// This functions as a countdown timer. This function converts the current
// time into the id of the image that is currently disappearing.
ButtonId PieNoonGame::CurrentlyAnimatingJoinImage(WorldTime time) const {
  const WorldTime time_in_state = time - state_entry_time_;
  const int seconds_in_state = time_in_state / 1000;
  const int id = ButtonId_Counter1 + seconds_in_state;
  const bool valid_id = id <= ButtonId_Counter5;
  return valid_id ? static_cast<ButtonId>(id) : ButtonId_Undefined;
}

PieNoonState PieNoonGame::UpdatePieNoonState() {
  const WorldTime time = CurrentWorldTime();
  // If a full screen fade is active.
  if (Fading()) {
    // If the fade hits the halfway point (opaque) enter the fade exit state.
    if (full_screen_fader_.Render(time)) {
      return fade_exit_state_;
    }
    // If the fade is complete, stop the transition process.
    if (full_screen_fader_.Finished(time)) {
      fade_exit_state_ = kUninitialized;
    }
  }
  switch (state_) {
    case kLoadingInitialMaterials: {
      const Config& config = GetConfig();
      if (matman_.FindMaterial(
            config.loading_material()->c_str())->textures()[0]->id() &&
          matman_.FindMaterial(
            config.loading_logo()->c_str())->textures()[0]->id() &&
          full_screen_fader_.material()->textures()[0]->id()) {
        // Fade in the loading screen.
        FadeToPieNoonState(kLoading, config.full_screen_fade_time(),
                         mathfu::kZeros4f, false);
      }
      break;
    }
    case kLoading: {
      const Config& config = GetConfig();
      // When we initialized assets, we kicked off a thread to load all
      // textures. Here we check if those have finished loading.
      // We also leave the loading screen up for a minimum amount of time.
      if (!Fading() && matman_.TryFinalize() &&
          (time - state_entry_time_) > config.min_loading_time()) {
        // If we've already displayed the tutorial before, jump straight to
        // the game. If we don't have the capability to record our previous
        // tutorial views, also jump straight to the game.
        int displayed_tutorial = ReadPreference("displayed_tutorial", 0, 1);
        const PieNoonState first_state =
            displayed_tutorial ? kFinished : kTutorial;

        // Fade out the loading screen and fade in the scene or tutorial.
        FadeToPieNoonState(first_state, config.full_screen_fade_time(),
                           mathfu::kZeros4f, true);
      }
      break;
    }
    case kJoining: {
      // Allow players to join with any key press.
      HandlePlayersJoining();

      // Count down by deactivating pies images.
      const ButtonId id = CurrentlyAnimatingJoinImage(time);

      // We've moved to animating a new pie.
      if (id != join_id_) {
        const Config& config = GetConfig();

        // Vanish the previous pie.
        StaticImage* prev_image = gui_menu_.FindImageById(join_id_);
        if (prev_image != nullptr) {
          prev_image->set_scale(mathfu::kZeros2f);
        }

        // Reset the impeller animation, if we've moved to a new image.
        impel::OvershootImpelInit init;
        impel::OvershootInitFromFlatBuffers(*config.join_impeller_def(), &init);
        join_impeller_.Initialize(init, &game_state_.impel_engine());
        join_impeller_.SetValue(config.join_impeller_start_value());
        join_impeller_.SetTargetValue(config.join_impeller_target_value());
        join_impeller_.SetVelocity(config.join_impeller_start_velocity());
        join_id_ = id;

        // Play a sound to aid with the countdown feeling.
        audio_engine_.PlaySound(SoundId_StartMatch);
      }

      // Scale the pie to show some pleasing movement.
      StaticImage* image = gui_menu_.FindImageById(id);
      if (image != nullptr) {
        image->set_scale(vec2(join_impeller_.Value()));
      }

      // After a few seconds, start the game.
      if (join_id_ == ButtonId_Undefined) {
        // Fade to the game
        FadeToPieNoonState(kPlaying, GetConfig().full_screen_fade_time(),
                           mathfu::kZeros4f, true);
      }
      break;
    }
    case kPlaying: {
      if (input_.GetButton(SDLK_AC_BACK).went_down() ||
          input_.GetButton(SDLK_p).went_down() ||
          input_.minimized_frame() == input_.frames()) {
        return kPaused;
      }
      if (game_state_.IsGameOver() &&
          stinger_channel_ != AudioEngine::kInvalidChannel &&
          !AudioEngine::Playing(stinger_channel_)) {
        return kFinished;
      }
      break;
    }
    case kPaused: {
      if (input_.GetButton(SDLK_AC_BACK).went_down()) {
        input_.exit_requested_ = true;
      }
      return HandleMenuButtons();
    }
    case kFinished: {
      if (input_.GetButton(SDLK_AC_BACK).went_down()) {
        const Config& config = GetConfig();
        const bool in_extras_menu = gui_menu_.menu_def() ==
                                    config.extras_screen_buttons();
        if (in_extras_menu) {
          gui_menu_.Setup(TitleScreenButtons(config), &matman_);
        } else {
          input_.exit_requested_ = true;
        }
      }
      return HandleMenuButtons();
    }
    case kTutorial: {
      const Config& config = GetConfig();
      const bool past_last_slide =
          tutorial_slide_index_ >=
          static_cast<int>(config.tutorial_slides()->Length());
      if (past_last_slide && !Fading()) {
        // Record that we've successfully displayed the tutorial so that we
        // don't display it again next time.
        WritePreference("displayed_tutorial", 1);

        // Fade out the tutorial screen and fade in the main menu.
        FadeToPieNoonState(kFinished, config.full_screen_fade_time(),
                           mathfu::kZeros4f, true);
      }
      break;
    }

    default:
      assert(false);
  }
  return state_;
}

void PieNoonGame::TransitionToPieNoonState(PieNoonState next_state) {
  assert(state_ != next_state);  // Must actually transition.
  const Config& config = GetConfig();

  switch (next_state) {
    case kLoadingInitialMaterials: {
      break;
    }
    case kLoading: {
      break;
    }
    case kJoining: {
      gui_menu_.Setup(config.join_screen_buttons(), &matman_);
      join_id_ = ButtonId_Undefined;
      game_state_.EnterJoiningMode();
      break;
    }
    case kPlaying: {
      gui_menu_.Setup(touch_controller_->character_id() == kNoCharacter ?
                      nullptr :
                      config.touchscreen_zones(), &matman_);

      if (state_ != kPaused) {
        audio_engine_.PlaySound(SoundId_StartMatch);
        audio_engine_.PlaySound(SoundId_MusicAction);
        ambience_channel_ = audio_engine_.PlaySound(SoundId_Ambience);
        game_state_.Reset();
      } else {
        audio_engine_.Pause(false);
      }
      break;
    }
    case kPaused: {
      gui_menu_.Setup(config.pause_screen_buttons(), &matman_);
      audio_engine_.Pause(true);
      break;
    }
    case kFinished: {
      gui_menu_.Setup(TitleScreenButtons(config), &matman_);
      if (ambience_channel_ != AudioEngine::kInvalidChannel) {
        audio_engine_.Stop(ambience_channel_);
      }
      stinger_channel_ = AudioEngine::kInvalidChannel;
      audio_engine_.PlaySound(SoundId_MusicMenu);
      for (size_t i = 0; i < game_state_.characters().size(); ++i) {
        auto& character = game_state_.characters()[i];
        if (character->controller()->controller_type() !=
            Controller::kTypeAI) {
          // Assign characters AI characters while the menu is up.
          // Players will have to press A again to get themselves re-assigned.
          // Find unused AI character:
          for (auto it = active_controllers_.begin();
                   it != active_controllers_.end(); ++it) {
            if ((*it)->controller_type() == Controller::kTypeAI &&
                (*it)->character_id() == kNoCharacter) {
              character->controller()->set_character_id(kNoCharacter);
              character->set_controller(it->get());
              (*it)->set_character_id(i);
              break;
            }
          }
          // There are as many AI controllers as there are players, so this
          // should never fail:
          assert(character->controller()->controller_type() ==
                 Controller::kTypeAI);
        }
      }
      // This should only happen if we just finished a game, not if we
      // end up in this state after loading.
      if (state_ == kPlaying) {
        UploadEvents();
        // For now, we always show leaderboards when a round ends:
        UploadAndShowLeaderboards();
      }
      break;
    }
    case kTutorial: {
      tutorial_slide_index_ = 0;
      LoadInitialTutorialSlides();
      break;
    }

    default:
      assert(false);
  }

  state_ = next_state;
  state_entry_time_ = prev_world_time_;
}

// Update the current game state and perform a state transition if requested.
PieNoonState PieNoonGame::UpdatePieNoonStateAndTransition() {
  const PieNoonState next_state = UpdatePieNoonState();
  if (next_state != state_) {
    TransitionToPieNoonState(next_state);
  }
  return next_state;
}

// Queue up a transition to the specified game state with a full screen fade
// between the states.
void PieNoonGame::FadeToPieNoonState(PieNoonState next_state,
                                 const WorldTime& fade_time,
                                 const mathfu::vec4& color,
                                 const bool fade_in) {
  if (!Fading()) {
    full_screen_fader_.Start(CurrentWorldTime(), fade_time, color, fade_in);
    fade_exit_state_ = next_state;
  }
}

#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
static GPGManager::GPGIds gpg_ids[] = {
  { "CgkI97yope0IEAIQAw", "CgkI97yope0IEAIQCg" },  // kWins
  { "CgkI97yope0IEAIQBA", "CgkI97yope0IEAIQCw" },  // kLosses
  { "CgkI97yope0IEAIQBQ", "CgkI97yope0IEAIQDA" },  // kDraws
  { "CgkI97yope0IEAIQAg", "CgkI97yope0IEAIQCQ" },  // kAttacks
  { "CgkI97yope0IEAIQBg", "CgkI97yope0IEAIQDQ" },  // kHits
  { "CgkI97yope0IEAIQBw", "CgkI97yope0IEAIQDg" },  // kBlocks
  { "CgkI97yope0IEAIQCA", "CgkI97yope0IEAIQDw" },  // kMisses
};
static_assert(sizeof(gpg_ids) / sizeof(GPGManager::GPGIds) ==
              kMaxStats, "update leaderboard_ids");
#endif

void PieNoonGame::UploadEvents() {
# ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  // Now upload all stats:
  // TODO: this assumes player 0 == the logged in player.
  Character* character = game_state_.characters()[0].get();
  for (int ps = kWins; ps < kMaxStats; ps++) {
    gpg_manager.IncrementEvent(gpg_ids[ps].event,
      character->GetStat(static_cast<PlayerStats>(ps)));
  }
  character->ResetStats();
# endif
}


// Called every frame to see if we've gotten any new achievements.
void PieNoonGame::CheckForNewAchievements() {
# ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  // We're assuming that player 0 is the one who's stats we care about.
  Character* character = game_state_.characters()[0].get();
  if (character->State() == StateId_Throwing &&
      character->state_last_update() != StateId_Throwing) {
    static const char* achievements[] = {
       "CgkI97yope0IEAIQEA",    // 100
       "CgkI97yope0IEAIQEQ",    // 250
       "CgkI97yope0IEAIQEg",    // 1000
       "CgkI97yope0IEAIQEw",    // 2500
       "CgkI97yope0IEAIQFA" };  // 10000
    int list_size = sizeof(achievements) / sizeof(char *);
    for (int i = 0; i < list_size; i++) {
      gpg_manager.IncrementAchievement(achievements[i]);
    }
  }
# endif
}

void PieNoonGame::UploadAndShowLeaderboards() {
# ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  gpg_manager.ShowLeaderboards(gpg_ids, sizeof(gpg_ids) /
                                        sizeof(GPGManager::GPGIds));
# endif
}

void PieNoonGame::UpdateGamepadControllers() {
#ifdef ANDROID_GAMEPAD
  // Iterate over list of currently known gamepads.
  for (auto it = input_.GamepadMap().begin();
       it != input_.GamepadMap().end(); ++it) {

    int device_id = it->first;
    // if we find one that doesn't have a player associated with it...
    if (gamepad_to_controller_map_.find(device_id) ==
        gamepad_to_controller_map_.end()) {
      GamepadController* controller = new GamepadController();
      controller->Initialize(&input_, device_id);
      gamepad_to_controller_map_[device_id] = AddController(controller);
    }
  }
#endif // ANDROID_GAMEPAD
}

// Returns the characterId of the first AI player we can find.
// Returns kNoCharacter if none were found.
CharacterId PieNoonGame::FindAiPlayer() {
  for (CharacterId char_id = 0;
       char_id < static_cast<CharacterId>(game_state_.characters().size());
       char_id++) {
    if (game_state_.characters()[char_id]->controller()->controller_type() ==
        Controller::kTypeAI) {
      return char_id;
    }
  }
  return kNoCharacter;
}

// Add a new controller into the list of known active controllers and assign
// an ID to it.
ControllerId PieNoonGame::AddController(Controller* new_controller) {
  for (ControllerId new_id = 0;
       new_id < static_cast<ControllerId>(active_controllers_.size());
       new_id++) {
    if (active_controllers_[new_id].get() == nullptr) {
      active_controllers_[new_id] = std::unique_ptr<Controller>(
          new_controller);
      return new_id;
    }
  }
  active_controllers_.push_back(std::unique_ptr<Controller>(new_controller));
  return active_controllers_.size() - 1;
}

// Returns a controller as specified by its ID
Controller* PieNoonGame::GetController(ControllerId id) {
  return (id >= 0 &&
          id < static_cast<ControllerId>(active_controllers_.size())) ?
        active_controllers_[id].get() : nullptr;
}

void PieNoonGame::HandlePlayersJoining(Controller* controller) {
  if (controller == nullptr ||
      controller->character_id() != kNoCharacter ||
      controller->controller_type() == Controller::kTypeAI)
    return;

  CharacterId open_slot = FindAiPlayer();
  if (open_slot == kNoCharacter)
    return;

  auto character = game_state_.characters()[open_slot].get();
  character->controller()->set_character_id(kNoCharacter);
  character->set_controller(controller);
  controller->set_character_id(open_slot);
  character->set_just_joined_game(true);
}

void PieNoonGame::HandlePlayersJoining() {
  for (auto it = active_controllers_.begin(); it != active_controllers_.end();
       ++it) {
    // Any input on a controller signals that the controller wants to join in.
    Controller* controller = it->get();
    const bool has_input = controller != nullptr &&
                           (controller->went_up() || controller->went_down());
    if(has_input) {
      HandlePlayersJoining(controller);
    }
  }
}

int PieNoonGame::ReadPreference(const char *key, int initial_value,
                                int failure_value) {
# ifdef __ANDROID__
  (void)failure_value;
  JNIEnv *env = reinterpret_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  jobject activity = reinterpret_cast<jobject>(SDL_AndroidGetActivity());
  jclass fpl_class = env->GetObjectClass(activity);
  jmethodID read_preference = env->GetMethodID(
      fpl_class, "ReadPreference", "(Ljava/lang/String;I)I");
  jstring text = env->NewStringUTF(key);
  int read = env->CallIntMethod(activity, read_preference, text, initial_value);
  env->DeleteLocalRef(fpl_class);
  env->DeleteLocalRef(text);
  env->DeleteLocalRef(activity);
  return read;
# else
  (void)key;
  (void)initial_value;
  return failure_value;
# endif
}

void PieNoonGame::WritePreference(const char *key, int value) {
# ifdef __ANDROID__
  JNIEnv *env = reinterpret_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  jobject activity = reinterpret_cast<jobject>(SDL_AndroidGetActivity());
  jclass fpl_class = env->GetObjectClass(activity);
  jmethodID write_preference = env->GetMethodID(
      fpl_class, "WritePreference", "(Ljava/lang/String;I)V");
  jstring text = env->NewStringUTF(key);
  env->CallVoidMethod(activity, write_preference, text, value);
  env->DeleteLocalRef(fpl_class);
  env->DeleteLocalRef(text);
  env->DeleteLocalRef(activity);
# else
  (void)key;
  (void)value;
# endif
}

static void DisplayDialogBox(const char* title, const char* text_file_name,
                             bool html) {
#ifdef __ANDROID__
  std::string dialog_text;
  if (!LoadFile(text_file_name, &dialog_text)) {
    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "can't load %s", text_file_name);
    return;
  }
  JNIEnv *env = reinterpret_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  jobject activity = reinterpret_cast<jobject>(SDL_AndroidGetActivity());
  jclass fpl_class = env->GetObjectClass(activity);
  jmethodID is_text_dialog_open = env->GetMethodID(
      fpl_class, "isTextDialogOpen", "()Z");
  jboolean open = env->CallBooleanMethod(activity, is_text_dialog_open);
  if (!open) {
    jmethodID show_text_dialog = env->GetMethodID(
        fpl_class, "showTextDialog",
        "(Ljava/lang/String;Ljava/lang/String;Z)V");
    jstring titlej = env->NewStringUTF(title);
    jstring dialog_textj = env->NewStringUTF(dialog_text.c_str());
    env->CallVoidMethod(activity, show_text_dialog, titlej, dialog_textj, html);
    env->DeleteLocalRef(dialog_textj);
    env->DeleteLocalRef(titlej);
  }
  env->DeleteLocalRef(fpl_class);
  env->DeleteLocalRef(activity);
#else
  (void)title;
  (void)text_file_name;
  (void)html;
#endif
}

PieNoonState PieNoonGame::HandleMenuButtons() {
  const ButtonId previous_focus = gui_menu_.GetFocus();
  for (size_t i = 0; i < active_controllers_.size(); i++) {
    Controller* controller = active_controllers_[i].get();
    if (controller != nullptr &&
        controller->controller_type() != Controller::kTypeAI) {
      gui_menu_.HandleControllerInput(controller->went_down(), i);
    }
  }
  if (previous_focus != gui_menu_.GetFocus()) {
    audio_engine_.PlaySound(SoundId_FocusMenuItem);
  }

  for (MenuSelection menu_selection = gui_menu_.GetRecentSelection();
       menu_selection.button_id != ButtonId_Undefined;
       menu_selection = gui_menu_.GetRecentSelection()) {
    switch (menu_selection.button_id) {
      case ButtonId_MenuSignIn:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Menu: SIGN IN/OUT");
        audio_engine_.PlaySound(SoundId_JoinMatch);
#       ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        gpg_manager.ToggleSignIn();
#       endif
        break;
      case ButtonId_MenuLicense:
        audio_engine_.PlaySound(SoundId_JoinMatch);
        DisplayDialogBox("Open Source Licenses", "licenses.txt", false);
        break;
      case ButtonId_MenuAbout:
        audio_engine_.PlaySound(SoundId_JoinMatch);
        DisplayDialogBox("About", "about.html", true);
        break;
      case ButtonId_MenuStart:
        if (state_ == kFinished) {
          audio_engine_.PlaySound(SoundId_JoinMatch);
          if (menu_selection.controller_id == kTouchController) {
            // When a touch controller exists, we assume it is the unique
            // input system for the game. We make the touch controller join the
            // game, and then start the game immediately.
            HandlePlayersJoining(touch_controller_);
            return kPlaying;
          }
          return kJoining;
        }
        break;
      case ButtonId_MenuResume:
        if (state_ == kPaused) {
          return kPlaying;
        }
        break;
      case ButtonId_MenuAchievements:
#       ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        gpg_manager.ShowAchievements();
#       endif
        break;
      case ButtonId_InvalidInput:
        audio_engine_.PlaySound(SoundId_InvalidInput);
        break;
      case ButtonId_MenuExtras: {
        const Config& config = GetConfig();
        gui_menu_.Setup(config.extras_screen_buttons(), &matman_);
        break;
      }
      case ButtonId_MenuBack: {
        const Config& config = GetConfig();
        gui_menu_.Setup(TitleScreenButtons(config), &matman_);
        break;
      }
      case ButtonId_MenuHowToPlay:
        return kTutorial;
      case ButtonId_MenuLeaderboard:
        UploadAndShowLeaderboards();
        break;
      default:
        break;
    }
  }
  return state_;
}

// Call AdvanceFrame on every controller that we're listening to
// and care about.  (Not all are connected to players, but we want
// to keep them up to date so we can check their inputs as needed.)
void PieNoonGame::UpdateControllers(WorldTime delta_time) {
  for (size_t i = 0; i < active_controllers_.size(); i++) {
    if (active_controllers_[i].get() != nullptr) {
      active_controllers_[i]->AdvanceFrame(delta_time);
    }
  }
}

void PieNoonGame::UpdateTouchButtons(WorldTime delta_time) {
  gui_menu_.AdvanceFrame(delta_time, &input_, vec2(renderer_.window_size()));

  // If we're playing the game, we have to send the menu events directly
  // to the touch controller, so it can act on them.
  if (state_ == kPlaying) {
    for (MenuSelection menu_selection = gui_menu_.GetRecentSelection();
         menu_selection.button_id != ButtonId_Undefined;
         menu_selection = gui_menu_.GetRecentSelection()) {
      touch_controller_->HandleTouchButtonInput(menu_selection.button_id, true);
    }
  }
}

ChannelId PieNoonGame::PlayStinger() {
  auto& characters = game_state_.characters();
  int player_winners = 0;
  int ai_winners = 0;
  for (size_t i = 0; i < characters.size(); ++i) {
    auto& character = characters[i];
    if (character->victory_state() == kVictorious) {
      if (character->controller()->controller_type() == Controller::kTypeAI) {
        ++ai_winners;
      } else {
        ++player_winners;
      }
    }
  }
  // If there's a single human winner, play the victory stinger.
  // If no humans won, play the lose stinger.
  // If more than one character won, or if they all lost, play the draw stinger.
  // This logic should work equally well for all game modes.
  if (player_winners == 1 && ai_winners == 0) {
    return audio_engine_.PlaySound(SoundId_StingerWin);
  } else if (player_winners == 0 && ai_winners > 0) {
    return audio_engine_.PlaySound(SoundId_StingerLose);
  } else {
    return audio_engine_.PlaySound(SoundId_StingerDraw);
  }
}

// Return the file name for the material at slide_index. If slide_index is
// invalid, return nullptr.
const char* PieNoonGame::TutorialSlideName(int slide_index) {
  const Config& config = GetConfig();
  const auto slides = config.tutorial_slides();
  const int num_slides = static_cast<int>(slides->Length());
  if (slide_index < 0 || slide_index >= num_slides)
    return nullptr;

  return slides->Get(slide_index)->c_str();
}

static bool ControllerHasPress(const Controller* controller) {
  return controller != nullptr &&
         controller->controller_type() != Controller::kTypeAI &&
         controller->went_down();
}

// Return true if a button press or touch screen touch has happened this frame.
bool PieNoonGame::AnyControllerPresses() {
  for (auto it = active_controllers_.begin(); it != active_controllers_.end();
       ++it) {
    const Controller* controller = it->get();
    if (ControllerHasPress(controller))
      return true;
  }
  return input_.GetPointerButton(0).went_down();
}

// Load into memory the tutorial slide at slide_index, if slide_index is valid.
// We preload some tutorial slides so that we can transition to them.
void PieNoonGame::LoadTutorialSlide(int slide_index) {
  const Config& config = GetConfig();
  const int num_slides = static_cast<int>(config.tutorial_slides()->Length());
  if (slide_index < 0 || slide_index >= num_slides)
    return;

  const char* slide_name = TutorialSlideName(slide_index);
  matman_.LoadMaterial(slide_name);
}

// Preload the initial few tutorial slides to prime the slide load-unload
// pipeline.
void PieNoonGame::LoadInitialTutorialSlides() {
  const Config& config = GetConfig();
  const int num_to_load =
      static_cast<int>(config.tutorial_num_future_slides_to_load());
  for (int slide_index = 0; slide_index < num_to_load; ++slide_index) {
    LoadTutorialSlide(slide_index);
  }
}

// Scale material by (aspect_ratio, 1) and then scale again so that it covers as
// much of the screen as possible.
void PieNoonGame::RenderInMiddleOfScreen(
    const mat4& ortho_mat, float aspect_ratio, Material* material) {
  // Calculate the texture scale. We want to fill the screen as much as we can,
  // but not change the aspect ratio. That means we letterbox either
  // horizontally or vertically.
  const vec2 window_size = vec2(renderer_.window_size());
  const vec2 texture_size = vec2(material->textures()[0]->size()) *
                            vec2(aspect_ratio, 1.0f);
  const vec2 scale_xy = window_size / texture_size;
  const float scale = std::min(scale_xy.x(), scale_xy.y());

  // Calculate the extreme corners of the texture, in 3D.
  const vec2 mid = 0.5f * window_size;
  const vec2 tex = 0.5f * scale * texture_size;
  const vec3 bottom_left(mid.x() - tex.x(), mid.y() + tex.y(), 0.0f);
  const vec3 top_right(mid.x() + tex.x(), mid.y() - tex.y(), 0.0f);

  // Render the texture to take up the whole screen.
  renderer_.model_view_projection() = ortho_mat;
  renderer_.color() = mathfu::kOnes4f;
  material->Set(renderer_);
  shader_textured_->Set(renderer_);
  Mesh::RenderAAQuadAlongX(bottom_left, top_right, vec2(0, 1), vec2(1, 0));
}

void PieNoonGame::Run() {
  // Initialize so that we don't sleep the first time through the loop.
  const Config& config = GetConfig();
  const WorldTime min_update_time = config.min_update_time();
  const WorldTime max_update_time = config.max_update_time();
  prev_world_time_ = CurrentWorldTime() - min_update_time;
  TransitionToPieNoonState(kLoadingInitialMaterials);
  game_state_.Reset();

  while (!input_.exit_requested_ &&
         !input_.GetButton(SDLK_ESCAPE).went_down()) {
    // Milliseconds elapsed since last update. To avoid burning through the
    // CPU, enforce a minimum time between updates. For example, if
    // min_update_time is 1, we will not exceed 1000Hz update time.
    const WorldTime world_time = CurrentWorldTime();
    const WorldTime delta_time = std::min(world_time - prev_world_time_,
                                          max_update_time);
    if (delta_time < min_update_time) {
      SDL_Delay(min_update_time - delta_time);
      continue;
    }

    // TODO: Can we move these to 'Render'?
    renderer_.AdvanceFrame(input_.minimized_);
    renderer_.ClearFrameBuffer(mathfu::kZeros4f);

    // Process input device messages since the last game loop.
    // Update render window size.
    input_.AdvanceFrame(&renderer_.window_size());

    UpdateGamepadControllers();
    UpdateControllers(delta_time);
    UpdateTouchButtons(delta_time);

    // Update the full screen fader dimensions.
    const auto res = renderer_.window_size();
    const auto ortho_mat = mathfu::OrthoHelper<float>(
        0.0f, static_cast<float>(res.x()), static_cast<float>(res.y()),
        0.0f, -1.0f, 1.0f);
    full_screen_fader_.set_ortho_mat(ortho_mat);
    full_screen_fader_.set_extents(res);

    // If we're all done loading, run & render the game as usual.
    switch (state_) {
      case kJoining:
      case kPlaying:
      case kPaused:
      case kFinished: {
        if (state_ != kPaused) {
          // Update game logic by a variable number of milliseconds.
          game_state_.AdvanceFrame(delta_time, &audio_engine_);
        }

        if (state_ == kPlaying &&
            stinger_channel_ == AudioEngine::kInvalidChannel &&
            game_state_.IsGameOver()) {
          game_state_.DetermineWinnersAndLosers();
          stinger_channel_ = PlayStinger();
        }

        // Update audio engine state.
        audio_engine_.AdvanceFrame(world_time);

        // Populate 'scene' from the game state--all the positions, orientations,
        // and renderable-ids (which specify materials) of the characters and
        // props. Also specify the camera matrix.
        game_state_.PopulateScene(&scene_);

        // Issue draw calls for the 'scene'.
        Render(scene_);

        // Render any UI/HUD/Splash on top.
        Render2DElements();

        // Output debug information.
        if (config.print_character_states()) {
          DebugPrintCharacterStates();
        }
        if (config.print_pie_states()) {
          DebugPrintPieStates();
        }
        if (config.allow_camera_movement()) {
          DebugCamera();
        }

        // Remember the real-world time from this frame.
        prev_world_time_ = world_time;

        // Advance to the next play state, if required.
        UpdatePieNoonStateAndTransition();

        // For testing,
        // we'll check if a sixth finger went down on the touch screen,
        // if so we update the leaderboards and show the UI:
        if (input_.GetButton(SDLK_POINTER6).went_down()) {
          UploadEvents();
          // For testing, show UI:
          UploadAndShowLeaderboards();
        }
  #     ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        gpg_manager.Update();
        WritePreference("logged_in", static_cast<int>(gpg_manager.LoggedIn()));
        CheckForNewAchievements();
  #     endif
        break;
      }

      case kLoading: {
        // If even the loading textures haven't loaded yet, remain on a black
        // screen, otherwise render the loading texture spinning and the
        // logo below.
        // Textures are still loading. Display a loading screen.
        auto spinmat = matman_.FindMaterial(
            config.loading_material()->c_str());
        auto logomat = matman_.FindMaterial(config.loading_logo()->c_str());
        assert(spinmat && logomat);
        assert(spinmat->textures()[0]->id() && logomat->textures()[0]->id());
        const auto mid = res / 2;
        const float time = static_cast<float>(world_time) /
          static_cast<float>(kMillisecondsPerSecond);
        auto rot_mat = mat3::RotationZ(time * 3.0f);
        renderer_.model_view_projection() = ortho_mat *
            mat4::FromTranslationVector(
                vec3(static_cast<float>(mid.x()),
                     static_cast<float>(mid.y()) * 0.7f, 0.0f)) *
            mat4::FromRotationMatrix(rot_mat);
        auto extend = vec2(spinmat->textures()[0]->size());
        renderer_.color() = mathfu::kOnes4f;
        spinmat->Set(renderer_);
        shader_textured_->Set(renderer_);
        Mesh::RenderAAQuadAlongX(
              vec3(-extend.x(),  extend.y(), 0),
              vec3( extend.x(), -extend.y(), 0),
              vec2(0, 1), vec2(1, 0));

        extend = vec2(logomat->textures()[0]->size()) / 10;
        renderer_.model_view_projection() = ortho_mat *
            mat4::FromTranslationVector(
                vec3(static_cast<float>(mid.x()),
                     static_cast<float>(res.y()) * 0.7f, 0.0f));
        renderer_.color() = mathfu::kOnes4f;
        logomat->Set(renderer_);
        shader_textured_->Set(renderer_);
        Mesh::RenderAAQuadAlongX(
              vec3(-extend.x(),  extend.y(), 0),
              vec3( extend.x(), -extend.y(), 0),
              vec2(0, 1), vec2(1, 0));
      } // Fallthrough

      case kLoadingInitialMaterials:
        // Finalize the materials that have been loaded thus far.
        matman_.TryFinalize();

        if (UpdatePieNoonStateAndTransition() == kFinished) {
          game_state_.Reset();
        }
        break;

      case kTutorial: {
        matman_.TryFinalize();

        const bool should_transition =
            full_screen_fader_.Finished(world_time) && AnyControllerPresses();
        if (should_transition) {
          // Start fade-out --> fade-in transition.
          full_screen_fader_.Start(
              world_time, config.tutorial_fade_time(), mathfu::kZeros4f, false);

          // Initiate asynchronous loading of a slide, several slides before
          // it is needed.
          const int future_slide_index =
              tutorial_slide_index_ +
              config.tutorial_num_future_slides_to_load();
          LoadTutorialSlide(future_slide_index);
        }

        // Draw the slide covering the entire screen.
        const char* slide_name = TutorialSlideName(tutorial_slide_index_);
        if (slide_name != nullptr) {
          Material* slide = matman_.FindMaterial(slide_name);
          if (slide->textures()[0]->id()) {
            RenderInMiddleOfScreen(ortho_mat, config.tutorial_aspect_ratio(),
                                   slide);
          }
        }

        // Overlay the darkening texture.
        if (!full_screen_fader_.Finished(world_time)) {
          const bool opaque = full_screen_fader_.Render(world_time);
          if (opaque) {
            // Unload current slide to save memory.
            matman_.UnloadMaterial(slide_name);

            // When completely dark, transition to the next slide.
            tutorial_slide_index_++;
          }
        }

        UpdatePieNoonStateAndTransition();
        break;
      }

      default:
        assert(false);
    }
  }
}

}  // pie_noon
}  // fpl

