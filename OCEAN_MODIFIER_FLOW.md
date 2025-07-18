# Ocean Modifier in Blender: Complete Code Flow

This document explains the complete code flow when creating and using an Ocean modifier in Blender, from UI interaction to the final mesh generation.

## Overview

The Ocean modifier in Blender creates realistic ocean surfaces using Statistical Wave Theory and FFT (Fast Fourier Transform). The system is divided into several layers:

1. **UI Layer**: User interface for adding and configuring the modifier
2. **RNA Layer**: Property system that connects UI to internal data
3. **Modifier Layer**: Blender's modifier system integration
4. **Simulation Layer**: Core ocean simulation algorithms

## 1. Adding an Ocean Modifier

### User Action
When a user clicks "Add Modifier" → "Ocean" in Blender's UI:

### Code Flow

1. **UI Menu** (`scripts/startup/bl_ui/properties_data_modifier.py`)
   ```python
   # Ocean modifier is listed under Physics modifiers
   col.operator_menu_enum("object.modifier_add", "type", icon='PHYSICS')
   ```

2. **Modifier Creation** (`source/blender/editors/object/object_modifier.cc`)
   ```cpp
   // modifier_add() function creates a new modifier
   ModifierData *md = BKE_modifier_new(type);
   BKE_modifiers_persistent_uid_init(ob, md);
   BLI_addtail(&ob->modifiers, md);
   ```

3. **Ocean Modifier Initialization** (`source/blender/modifiers/intern/MOD_ocean.cc`)
   ```cpp
   static void init_data(ModifierData *md)
   {
     OceanModifierData *omd = (OceanModifierData *)md;
     
     // Set default values
     omd->resolution = 7;
     omd->spatial_size = 50;
     omd->wind_velocity = 30.0;
     omd->smallest_wave = 0.01;
     omd->wave_alignment = 0.0;
     omd->depth = 200.0;
     omd->wave_direction = 0.0;
     omd->chop_amount = 1.0;
     // ... more defaults
     
     // Create ocean simulation object
     omd->ocean = BKE_ocean_add();
     BKE_ocean_init_from_modifier(omd->ocean, omd, omd->resolution);
   }
   ```

## 2. Ocean Simulation Initialization

### Core Ocean Creation (`source/blender/blenkernel/intern/ocean.cc`)

1. **Ocean Object Creation**
   ```cpp
   Ocean *BKE_ocean_add()
   {
     Ocean *oc = MEM_callocN<Ocean>("ocean sim data");
     BLI_rw_mutex_init(&oc->oceanmutex);
     return oc;
   }
   ```

2. **Ocean Initialization**
   ```cpp
   bool BKE_ocean_init_from_modifier(Ocean *ocean, OceanModifierData const *omd, const int resolution)
   {
     // Extract parameters from modifier
     short do_heightfield = true;
     short do_chop = (omd->chop_amount > 0);
     short do_normals = (omd->flag & MOD_OCEAN_GENERATE_NORMALS);
     short do_jacobian = (omd->flag & MOD_OCEAN_GENERATE_FOAM);
     
     // Initialize ocean with parameters
     return BKE_ocean_init(ocean,
                          resolution * resolution,  // M
                          resolution * resolution,  // N
                          omd->spatial_size,       // Lx
                          omd->spatial_size,       // Lz
                          omd->wind_velocity,      // V
                          omd->smallest_wave,      // l
                          1.0,                     // A
                          omd->wave_direction,     // w
                          omd->damp,              // damp
                          omd->wave_alignment,     // alignment
                          omd->depth,             // depth
                          omd->time,              // time
                          omd->spectrum,          // spectrum type
                          omd->fetch_jonswap,
                          omd->sharpen_peak_jonswap,
                          do_heightfield,
                          do_chop,
                          do_spray,
                          do_normals,
                          do_jacobian,
                          omd->seed);
   }
   ```

3. **Wave Spectrum Generation**
   ```cpp
   // In BKE_ocean_init()
   // For each grid point in frequency space:
   for (i = 0; i < o->_M; i++) {
     for (j = 0; j < o->_N; j++) {
       // Generate random phase
       float r1 = gaussRand(rng);
       float r2 = gaussRand(rng);
       
       // Apply wave spectrum (e.g., Phillips spectrum)
       mul_complex_f(o->_h0[i * o->_N + j], 
                     r1r2, 
                     sqrt(Ph(o, o->_kx[i], o->_kz[j]) / 2.0f));
     }
   }
   ```

## 3. Property System (RNA)

### Property Definitions (`source/blender/makesrna/intern/rna_modifier.cc`)

When user changes a property in the UI:

```cpp
static void rna_def_modifier_ocean(BlenderRNA *brna)
{
  PropertyRNA *prop;
  
  // Resolution property
  prop = RNA_def_property(srna, "resolution", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "resolution");
  RNA_def_property_range(prop, 1, 1024);
  RNA_def_property_ui_range(prop, 1, 32, 1, -1);
  RNA_def_property_update(prop, 0, "rna_OceanModifier_init_update");
  
  // Wind velocity property
  prop = RNA_def_property(srna, "wind_velocity", PROP_FLOAT, PROP_VELOCITY);
  RNA_def_property_float_sdna(prop, nullptr, "wind_velocity");
  RNA_def_property_update(prop, 0, "rna_OceanModifier_init_update");
  
  // ... more properties
}
```

### Update Callback
```cpp
static void rna_OceanModifier_init_update(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  OceanModifierData *omd = (OceanModifierData *)ptr->data;
  
  // Reinitialize ocean when properties change
  BKE_ocean_free_data(omd->ocean);
  BKE_ocean_init_from_modifier(omd->ocean, omd, omd->resolution);
}
```

## 4. Mesh Generation/Deformation

### During Viewport Update or Render

1. **Modifier Evaluation** (`source/blender/modifiers/intern/MOD_ocean.cc`)
   ```cpp
   static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
   {
     OceanModifierData *omd = (OceanModifierData *)md;
     
     // Ensure ocean is initialized
     if (BKE_ocean_ensure(omd, resolution)) {
       // Ocean was recreated
     }
     
     // Generate or deform mesh
     return doOcean(md, ctx, mesh);
   }
   ```

2. **Ocean Simulation** (`doOcean()` function)
   ```cpp
   static Mesh *doOcean(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
   {
     // Get current frame time
     float current_time = DEG_get_ctime(ctx->depsgraph);
     
     // Simulate ocean at current time
     simulate_ocean_modifier(omd);
     
     if (omd->flag & MOD_OCEAN_GENERATE_GEOMETRY) {
       // Generate new mesh from ocean grid
       result = generate_ocean_mesh(omd, resolution);
     } else {
       // Displace existing mesh vertices
       result = displace_ocean_mesh(omd, mesh);
     }
     
     return result;
   }
   ```

3. **Ocean Time Evolution**
   ```cpp
   static void simulate_ocean_modifier(OceanModifierData *omd)
   {
     if (!omd->cached) {
       // Run simulation
       BKE_ocean_simulate(omd->ocean, omd->time, omd->scale, omd->chop_amount);
     } else {
       // Load from cache
       BKE_ocean_simulate_cache(omd->oceancache, frame);
     }
   }
   ```

4. **Core Simulation** (`BKE_ocean_simulate()`)
   ```cpp
   void BKE_ocean_simulate(Ocean *o, float t, float scale, float chop_amount)
   {
     // Phase 1: Compute time-evolved wave amplitudes
     // For each frequency component:
     // h(k,t) = h0(k)*e^(i*omega*t) + h0*(-k)*e^(-i*omega*t)
     BLI_task_parallel_range(0, o->_M, &osd, ocean_compute_htilda, &settings);
     
     // Phase 2: Transform to spatial domain using FFT
     if (o->_do_disp_y) {
       ocean_compute_displacement_y();  // Vertical displacement
     }
     if (o->_do_chop) {
       ocean_compute_displacement_x();  // Horizontal displacement X
       ocean_compute_displacement_z();  // Horizontal displacement Z
     }
     if (o->_do_jacobian) {
       ocean_compute_jacobian_jxx();    // For foam generation
       ocean_compute_jacobian_jzz();
       ocean_compute_jacobian_jxz();
     }
     if (o->_do_normals) {
       ocean_compute_normal_x();        // Surface normals
       ocean_compute_normal_z();
     }
   }
   ```

## 5. Mesh Generation Details

### Generate Mode
```cpp
// In generate_ocean_geometry()
for (y = 0; y <= res_y; y++) {
  for (x = 0; x <= res_x; x++) {
    // Evaluate ocean at this grid point
    BKE_ocean_eval_ij(omd->ocean, &ocr, x, y);
    
    // Set vertex position
    co = verts[x + y * (res_x + 1)].co;
    co[0] = ox + (x * sx);
    co[1] = oy + ocr.disp[1];  // Height displacement
    co[2] = oz + (y * sy);
    
    if (omd->chop_amount > 0.0f) {
      co[0] += ocr.disp[0];    // Choppy X displacement
      co[2] += ocr.disp[2];    // Choppy Z displacement
    }
  }
}
```

### Displace Mode
```cpp
// For each vertex in existing mesh:
for (i = 0; i < verts_num; i++) {
  // Map vertex position to ocean UV
  float u = (co[0] - omd->location[0]) / omd->spatial_size;
  float v = (co[2] - omd->location[2]) / omd->spatial_size;
  
  // Evaluate ocean at UV
  BKE_ocean_eval_uv(omd->ocean, &ocr, u, v);
  
  // Apply displacement
  co[1] += ocr.disp[1] * omd->scale;
}
```

## 6. Caching/Baking System

### Bake Operator
```cpp
// User clicks "Bake Ocean" button
static int ocean_bake_exec(bContext *C, wmOperator *op)
{
  OceanCache *och = BKE_ocean_init_cache(
      omd->cachepath, omd->bakestart, omd->bakeend,
      omd->wave_scale, omd->chop_amount,
      omd->foam_coverage, omd->foam_fade, resolution);
  
  // Bake all frames
  BKE_ocean_bake(omd->ocean, och, ocean_bake_update, &obd);
}
```

### Baking Process
```cpp
// For each frame:
for (f = start; f <= end; f++) {
  // Simulate ocean
  BKE_ocean_simulate(ocean, time, scale, chop);
  
  // Evaluate at each grid point
  for (y = 0; y < res_y; y++) {
    for (x = 0; x < res_x; x++) {
      BKE_ocean_eval_ij(ocean, &ocr, x, y);
      
      // Store in image buffers
      displacement_buffer[idx] = ocr.disp;
      foam_buffer[idx] = ocr.foam;
      normal_buffer[idx] = ocr.normal;
    }
  }
  
  // Save as OpenEXR files
  BKE_imbuf_write(ibuf_disp, "disp_0001.exr");
  BKE_imbuf_write(ibuf_foam, "foam_0001.exr");
  // etc...
}
```

## Summary

The Ocean modifier flow can be summarized as:

1. **User adds modifier** → Creates `OceanModifierData` and `Ocean` objects
2. **Properties changed** → RNA update callbacks reinitialize ocean
3. **Frame changes** → Modifier evaluation triggered
4. **Simulation runs** → FFT transforms wave data to spatial domain
5. **Mesh updated** → Either generated from scratch or vertices displaced
6. **Optional: Baking** → Pre-compute and save results for faster playback

The system is designed to be modular, with clear separation between:
- UI/Properties (RNA system)
- Modifier integration (MOD_ocean.cc)
- Core simulation (ocean.cc)
- Caching system (ocean baking)

This architecture allows the ocean simulation to be used independently of the modifier system if needed, while providing seamless integration with Blender's standard workflows.