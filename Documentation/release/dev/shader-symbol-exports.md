## shader-symbol-exports

VTK previously exported a lot of its shader strings from its libraries. Now
only those that are available through installed headers are available. These
include:

- `vtkTextureObjectVS` from `VTK::RenderingOpenGL2`
- `vtkCompositeZPassFS` from `VTK::RenderingParallel`
