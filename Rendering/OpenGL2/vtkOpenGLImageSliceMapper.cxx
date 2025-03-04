/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkOpenGLImageSliceMapper.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkOpenGLImageSliceMapper.h"

#include "vtk_glew.h"

#include "vtkDataArray.h"
#include "vtkGarbageCollector.h"
#include "vtkImageData.h"
#include "vtkImageProperty.h"
#include "vtkImageSlice.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkLookupTable.h"
#include "vtkMapper.h"
#include "vtkMath.h"
#include "vtkMatrix3x3.h"
#include "vtkMatrix4x4.h"
#include "vtkObjectFactory.h"
#include "vtkOpenGLCamera.h"
#include "vtkOpenGLRenderWindow.h"
#include "vtkOpenGLRenderer.h"
#include "vtkOpenGLState.h"
#include "vtkPoints.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkTemplateAliasMacro.h"
#include "vtkTimerLog.h"

#include "vtkActor.h"
#include "vtkCellArray.h"
#include "vtkFloatArray.h"
#include "vtkNew.h"
#include "vtkOpenGLPolyDataMapper.h"
#include "vtkPointData.h"
#include "vtkPolyData.h"
#include "vtkProperty.h"
#include "vtkTexture.h"
#include "vtkTrivialProducer.h"

#include <cmath>

#include "vtkOpenGLError.h"

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkOpenGLImageSliceMapper);

//------------------------------------------------------------------------------
// Initializes an instance, generates a unique index.
vtkOpenGLImageSliceMapper::vtkOpenGLImageSliceMapper()
{
  // setup the polygon mapper
  {
    vtkNew<vtkPolyData> polydata;
    vtkNew<vtkPoints> points;
    points->SetNumberOfPoints(4);
    polydata->SetPoints(points);

    vtkNew<vtkCellArray> tris;
    polydata->SetPolys(tris);

    vtkNew<vtkFloatArray> tcoords;
    tcoords->SetNumberOfComponents(2);
    tcoords->SetNumberOfTuples(4);
    polydata->GetPointData()->SetTCoords(tcoords);

    vtkNew<vtkTrivialProducer> prod;
    prod->SetOutput(polydata);
    vtkNew<vtkOpenGLPolyDataMapper> polyDataMapper;
    polyDataMapper->SetInputConnection(prod->GetOutputPort());
    this->PolyDataActor = vtkActor::New();
    this->PolyDataActor->SetMapper(polyDataMapper);
    vtkNew<vtkTexture> texture;
    texture->RepeatOff();
    this->PolyDataActor->SetTexture(texture);
  }

  // setup the backing polygon mapper
  {
    vtkNew<vtkPolyData> polydata;
    vtkNew<vtkPoints> points;
    points->SetNumberOfPoints(4);
    polydata->SetPoints(points);

    vtkNew<vtkCellArray> tris;
    polydata->SetPolys(tris);

    vtkNew<vtkTrivialProducer> prod;
    prod->SetOutput(polydata);
    vtkNew<vtkOpenGLPolyDataMapper> polyDataMapper;
    polyDataMapper->SetInputConnection(prod->GetOutputPort());
    this->BackingPolyDataActor = vtkActor::New();
    this->BackingPolyDataActor->SetMapper(polyDataMapper);
  }

  // setup the background polygon mapper
  {
    vtkNew<vtkPolyData> polydata;
    vtkNew<vtkPoints> points;
    points->SetNumberOfPoints(10);
    polydata->SetPoints(points);

    vtkNew<vtkCellArray> tris;
    polydata->SetPolys(tris);

    vtkNew<vtkTrivialProducer> prod;
    prod->SetOutput(polydata);
    vtkNew<vtkOpenGLPolyDataMapper> polyDataMapper;
    polyDataMapper->SetInputConnection(prod->GetOutputPort());
    this->BackgroundPolyDataActor = vtkActor::New();
    this->BackgroundPolyDataActor->SetMapper(polyDataMapper);
  }

  this->RenderWindow = nullptr;
  this->TextureSize[0] = 0;
  this->TextureSize[1] = 0;
  this->TextureBytesPerPixel = 1;

  this->LastOrientation = -1;
  this->LastSliceNumber = VTK_INT_MAX;
}

//------------------------------------------------------------------------------
vtkOpenGLImageSliceMapper::~vtkOpenGLImageSliceMapper()
{
  this->RenderWindow = nullptr;
  this->BackgroundPolyDataActor->UnRegister(this);
  this->BackingPolyDataActor->UnRegister(this);
  this->PolyDataActor->UnRegister(this);
}

//------------------------------------------------------------------------------
// Release the graphics resources used by this texture.
void vtkOpenGLImageSliceMapper::ReleaseGraphicsResources(vtkWindow* renWin)
{
  this->BackgroundPolyDataActor->ReleaseGraphicsResources(renWin);
  this->BackingPolyDataActor->ReleaseGraphicsResources(renWin);
  this->PolyDataActor->ReleaseGraphicsResources(renWin);

  this->RenderWindow = nullptr;
  this->Modified();
}

//------------------------------------------------------------------------------
// Subdivide the image until the pieces fit into texture memory
void vtkOpenGLImageSliceMapper::RecursiveRenderTexturedPolygon(
  vtkRenderer* ren, vtkImageProperty* property, vtkImageData* input, int extent[6], bool recursive)
{
  int xdim, ydim;
  int imageSize[2];
  int textureSize[2];

  // compute image size and texture size from extent
  this->ComputeTextureSize(extent, xdim, ydim, imageSize, textureSize);

  // Check if we can fit this texture in memory
  if (this->TextureSizeOK(textureSize, ren))
  {
    // We can fit it - render
    this->RenderTexturedPolygon(ren, property, input, extent, recursive);
  }

  // If the texture does not fit, then subdivide and render
  // each half.  Unless the graphics card couldn't handle
  // a texture a small as 256x256, because if it can't handle
  // that, then something has gone horribly wrong.
  else if (textureSize[0] > 256 || textureSize[1] > 256)
  {
    int subExtent[6];
    subExtent[0] = extent[0];
    subExtent[1] = extent[1];
    subExtent[2] = extent[2];
    subExtent[3] = extent[3];
    subExtent[4] = extent[4];
    subExtent[5] = extent[5];

    // Which is larger, x or y?
    int idx = ydim;
    int tsize = textureSize[1];
    if (textureSize[0] > textureSize[1])
    {
      idx = xdim;
      tsize = textureSize[0];
    }

    // Divide size by two
    tsize /= 2;

    // Render each half recursively
    subExtent[idx * 2] = extent[idx * 2];
    subExtent[idx * 2 + 1] = extent[idx * 2] + tsize - 1;
    this->RecursiveRenderTexturedPolygon(ren, property, input, subExtent, true);

    subExtent[idx * 2] = subExtent[idx * 2] + tsize;
    subExtent[idx * 2 + 1] = extent[idx * 2 + 1];
    this->RecursiveRenderTexturedPolygon(ren, property, input, subExtent, true);
  }
}

//------------------------------------------------------------------------------
// Load the given image extent into a texture and render it
void vtkOpenGLImageSliceMapper::RenderTexturedPolygon(
  vtkRenderer* ren, vtkImageProperty* property, vtkImageData* input, int extent[6], bool recursive)
{
  // get the previous texture load time
  vtkMTimeType loadTime = this->LoadTime.GetMTime();

  // the render window, needed for state information
  vtkOpenGLRenderWindow* renWin = static_cast<vtkOpenGLRenderWindow*>(ren->GetRenderWindow());

  bool reuseTexture = true;

  // if context has changed, verify context capabilities
  if (renWin != this->RenderWindow || renWin->GetContextCreationTime() > loadTime)
  {
    this->RenderWindow = renWin;
    reuseTexture = false;
  }

  vtkOpenGLClearErrorMacro();

  // get information about the image
  int xdim, ydim; // orientation of texture wrt input image
  vtkImageSliceMapper::GetDimensionIndices(this->Orientation, xdim, ydim);

  // verify that the orientation and slice has not changed
  bool orientationChanged = (this->Orientation != this->LastOrientation);
  this->LastOrientation = this->Orientation;
  bool sliceChanged = (this->SliceNumber != this->LastSliceNumber);
  this->LastSliceNumber = this->SliceNumber;

  // get the mtime of the property, including the lookup table
  vtkMTimeType propertyMTime = 0;
  if (property)
  {
    propertyMTime = property->GetMTime();
    if (!this->PassColorData)
    {
      vtkScalarsToColors* table = property->GetLookupTable();
      if (table)
      {
        vtkMTimeType mtime = table->GetMTime();
        if (mtime > propertyMTime)
        {
          propertyMTime = mtime;
        }
      }
    }
  }

  // need to reload the texture
  if (this->Superclass::GetMTime() > loadTime || propertyMTime > loadTime ||
    input->GetMTime() > loadTime || orientationChanged || sliceChanged || recursive)
  {
    // get the data to load as a texture
    int xsize;
    int ysize;
    int bytesPerPixel;

    // whether to try to use the input data directly as the texture
    bool reuseData = true;

    // generate the data to be used as a texture
    unsigned char* data = this->MakeTextureData((this->PassColorData ? nullptr : property), input,
      extent, xsize, ysize, bytesPerPixel, reuseTexture, reuseData);

    this->TextureSize[0] = xsize;
    this->TextureSize[1] = ysize;
    this->TextureBytesPerPixel = bytesPerPixel;

    vtkImageData* id = vtkImageData::New();
    id->SetExtent(0, xsize - 1, 0, ysize - 1, 0, 0);
    vtkUnsignedCharArray* uca = vtkUnsignedCharArray::New();
    uca->SetNumberOfComponents(bytesPerPixel);
    uca->SetArray(
      data, xsize * ysize * bytesPerPixel, reuseData, vtkAbstractArray::VTK_DATA_ARRAY_DELETE);
    id->GetPointData()->SetScalars(uca);
    uca->Delete();

    this->PolyDataActor->GetTexture()->SetInputData(id);
    id->Delete();

    if (property->GetInterpolationType() == VTK_NEAREST_INTERPOLATION && !this->ExactPixelMatch)
    {
      this->PolyDataActor->GetTexture()->InterpolateOff();
    }
    else
    {
      this->PolyDataActor->GetTexture()->InterpolateOn();
    }

    this->PolyDataActor->GetTexture()->EdgeClampOn();

    // modify the load time to the current time
    this->LoadTime.Modified();
  }

  vtkPoints* points = this->Points;
  if (this->ExactPixelMatch && this->SliceFacesCamera)
  {
    points = nullptr;
  }

  this->RenderPolygon(this->PolyDataActor, points, extent, ren);

  if (this->Background)
  {
    double ambient = property->GetAmbient();
    double diffuse = property->GetDiffuse();

    double bkcolor[4];
    this->GetBackgroundColor(property, bkcolor);
    vtkProperty* pdProp = this->BackgroundPolyDataActor->GetProperty();
    pdProp->SetAmbient(ambient);
    pdProp->SetDiffuse(diffuse);
    pdProp->SetColor(bkcolor[0], bkcolor[1], bkcolor[2]);
    this->RenderBackground(this->BackgroundPolyDataActor, points, extent, ren);
  }

  vtkOpenGLCheckErrorMacro("failed after RenderTexturedPolygon");
}

//------------------------------------------------------------------------------
// Render the polygon that displays the image data
void vtkOpenGLImageSliceMapper::RenderPolygon(
  vtkActor* actor, vtkPoints* points, const int extent[6], vtkRenderer* ren)
{
  vtkOpenGLClearErrorMacro();

  bool textured = (actor->GetTexture() != nullptr);
  vtkPolyData* poly = vtkPolyDataMapper::SafeDownCast(actor->GetMapper())->GetInput();
  vtkPoints* polyPoints = poly->GetPoints();
  vtkCellArray* tris = poly->GetPolys();
  vtkDataArray* polyTCoords = poly->GetPointData()->GetTCoords();

  // do we need to rebuild the cell array?
  int numTris = 2;
  if (points)
  {
    numTris = (points->GetNumberOfPoints() - 2);
  }
  if (tris->GetNumberOfConnectivityIds() != 3 * numTris)
  {
    tris->Initialize();
    tris->AllocateEstimate(numTris, 3);
    // this wacky code below works for 2 and 4 triangles at least
    for (vtkIdType i = 0; i < numTris; i++)
    {
      tris->InsertNextCell(3);
      tris->InsertCellPoint(numTris + 1 - (i + 1) / 2);
      tris->InsertCellPoint(i / 2);
      tris->InsertCellPoint((i % 2 == 0) ? numTris - i / 2 : i / 2 + 1);
    }
    tris->Modified();
  }

  // now rebuild the points/tcoords as needed
  if (!points)
  {
    double coords[12], tcoords[8];
    this->MakeTextureGeometry(extent, coords, tcoords);

    polyPoints->SetNumberOfPoints(4);
    if (textured)
    {
      polyTCoords->SetNumberOfTuples(4);
    }
    for (int i = 0; i < 4; i++)
    {
      polyPoints->SetPoint(i, coords[3 * i], coords[3 * i + 1], coords[3 * i + 2]);
      if (textured)
      {
        polyTCoords->SetTuple(i, &tcoords[2 * i]);
      }
    }
    polyPoints->Modified();
    if (textured)
    {
      polyTCoords->Modified();
    }
  }
  else if (points->GetNumberOfPoints())
  {
    int xdim, ydim;
    vtkImageSliceMapper::GetDimensionIndices(this->Orientation, xdim, ydim);
    double* origin = this->DataOrigin;
    double* spacing = this->DataSpacing;
    double xshift = -(0.5 - extent[2 * xdim]) * spacing[xdim];
    double xscale = this->TextureSize[xdim] * spacing[xdim];
    double yshift = -(0.5 - extent[2 * ydim]) * spacing[ydim];
    double yscale = this->TextureSize[ydim] * spacing[ydim];
    vtkIdType ncoords = points->GetNumberOfPoints();
    double coord[3];
    double tcoord[2];
    double invDirection[9];

    polyPoints->DeepCopy(points);
    if (textured)
    {
      vtkMatrix3x3::Invert(this->DataDirection, invDirection);
      polyTCoords->SetNumberOfTuples(ncoords);
    }

    for (vtkIdType i = 0; i < ncoords; i++)
    {
      if (textured)
      {
        // convert points from 3D model coords to 2D texture coords
        points->GetPoint(i, coord);
        vtkMath::Subtract(coord, origin, coord);
        vtkMatrix3x3::MultiplyPoint(invDirection, coord, coord);
        tcoord[0] = (coord[0] - xshift) / xscale;
        tcoord[1] = (coord[1] - yshift) / yscale;
        polyTCoords->SetTuple(i, tcoord);
      }
    }
    if (textured)
    {
      polyTCoords->Modified();
    }
  }
  else // no polygon to render
  {
    return;
  }

  if (textured)
  {
    actor->GetTexture()->Render(ren);
  }
  actor->GetMapper()->SetClippingPlanes(this->GetClippingPlanes());
  actor->GetMapper()->Render(ren, actor);
  if (textured)
  {
    actor->GetTexture()->PostRender(ren);
  }

  vtkOpenGLCheckErrorMacro("failed after RenderPolygon");
}

//------------------------------------------------------------------------------
// Render a wide black border around the polygon, wide enough to fill
// the entire viewport.
void vtkOpenGLImageSliceMapper::RenderBackground(
  vtkActor* actor, vtkPoints* points, const int extent[6], vtkRenderer* ren)
{
  vtkOpenGLClearErrorMacro();

  vtkPolyData* poly = vtkPolyDataMapper::SafeDownCast(actor->GetMapper())->GetInput();
  vtkPoints* polyPoints = poly->GetPoints();
  vtkCellArray* tris = poly->GetPolys();

  static double borderThickness = 1e6;
  int xdim, ydim;
  vtkImageSliceMapper::GetDimensionIndices(this->Orientation, xdim, ydim);

  if (!points)
  {
    double coords[15], tcoords[10], center[3];
    this->MakeTextureGeometry(extent, coords, tcoords);
    coords[12] = coords[0];
    coords[13] = coords[1];
    coords[14] = coords[2];

    center[0] = 0.25 * (coords[0] + coords[3] + coords[6] + coords[9]);
    center[1] = 0.25 * (coords[1] + coords[4] + coords[7] + coords[10]);
    center[2] = 0.25 * (coords[2] + coords[5] + coords[8] + coords[11]);

    // render 4 sides
    tris->Initialize();
    polyPoints->SetNumberOfPoints(10);
    for (int side = 0; side < 4; side++)
    {
      tris->InsertNextCell(3);
      tris->InsertCellPoint(side);
      tris->InsertCellPoint(side + 5);
      tris->InsertCellPoint(side + 1);
      tris->InsertNextCell(3);
      tris->InsertCellPoint(side + 1);
      tris->InsertCellPoint(side + 5);
      tris->InsertCellPoint(side + 6);
    }

    for (int side = 0; side < 5; side++)
    {
      polyPoints->SetPoint(side, coords[3 * side], coords[3 * side + 1], coords[3 * side + 2]);

      double dx = coords[3 * side + xdim] - center[xdim];
      double sx = (dx >= 0 ? 1 : -1);
      double dy = coords[3 * side + ydim] - center[ydim];
      double sy = (dy >= 0 ? 1 : -1);
      coords[3 * side + xdim] += borderThickness * sx;
      coords[3 * side + ydim] += borderThickness * sy;

      polyPoints->SetPoint(side + 5, coords[3 * side], coords[3 * side + 1], coords[3 * side + 2]);
    }
  }
  else if (points->GetNumberOfPoints())
  {
    vtkIdType ncoords = points->GetNumberOfPoints();
    double coord[3], coord1[3];

    points->GetPoint(ncoords - 1, coord1);
    points->GetPoint(0, coord);
    double dx0 = coord[0] - coord1[0];
    double dy0 = coord[1] - coord1[1];
    double r = sqrt(dx0 * dx0 + dy0 * dy0);
    dx0 /= r;
    dy0 /= r;

    tris->Initialize();
    polyPoints->SetNumberOfPoints(ncoords * 2 + 2);

    for (vtkIdType i = 0; i < ncoords; i++)
    {
      tris->InsertNextCell(3);
      tris->InsertCellPoint(i * 2);
      tris->InsertCellPoint(i * 2 + 1);
      tris->InsertCellPoint(i * 2 + 2);
      tris->InsertNextCell(3);
      tris->InsertCellPoint(i * 2 + 2);
      tris->InsertCellPoint(i * 2 + 1);
      tris->InsertCellPoint(i * 2 + 3);
    }

    for (vtkIdType i = 0; i <= ncoords; i++)
    {
      polyPoints->SetPoint(i * 2, coord);

      points->GetPoint(((i + 1) % ncoords), coord1);
      double dx1 = coord1[0] - coord[0];
      double dy1 = coord1[1] - coord[1];
      r = sqrt(dx1 * dx1 + dy1 * dy1);
      dx1 /= r;
      dy1 /= r;

      double t;
      if (fabs(dx0 + dx1) > fabs(dy0 + dy1))
      {
        t = (dy1 - dy0) / (dx0 + dx1);
      }
      else
      {
        t = (dx0 - dx1) / (dy0 + dy1);
      }
      coord[0] += (t * dx0 + dy0) * borderThickness;
      coord[1] += (t * dy0 - dx0) * borderThickness;

      polyPoints->SetPoint(i * 2 + 1, coord);

      coord[0] = coord1[0];
      coord[1] = coord1[1];
      dx0 = dx1;
      dy0 = dy1;
    }
  }
  else // no polygon to render
  {
    return;
  }

  polyPoints->GetData()->Modified();
  tris->Modified();
  actor->GetMapper()->SetClippingPlanes(this->GetClippingPlanes());
  actor->GetMapper()->Render(ren, actor);

  vtkOpenGLCheckErrorMacro("failed after RenderBackground");
}

//------------------------------------------------------------------------------
void vtkOpenGLImageSliceMapper::ComputeTextureSize(
  const int extent[6], int& xdim, int& ydim, int imageSize[2], int textureSize[2])
{
  // find dimension indices that will correspond to the
  // columns and rows of the 2D texture
  vtkImageSliceMapper::GetDimensionIndices(this->Orientation, xdim, ydim);

  // compute the image dimensions
  imageSize[0] = (extent[xdim * 2 + 1] - extent[xdim * 2] + 1);
  imageSize[1] = (extent[ydim * 2 + 1] - extent[ydim * 2] + 1);

  textureSize[0] = imageSize[0];
  textureSize[1] = imageSize[1];
}

//------------------------------------------------------------------------------
// Determine if a given texture size is supported by the video card
bool vtkOpenGLImageSliceMapper::TextureSizeOK(const int size[2], vtkRenderer* ren)
{
  vtkOpenGLRenderWindow* renWin = vtkOpenGLRenderWindow::SafeDownCast(ren->GetRenderWindow());
  vtkOpenGLState* ostate = renWin->GetState();

  // First ask OpenGL what the max texture size is
  GLint maxSize;
  ostate->vtkglGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
  // if it does fit, we will render it later
  return size[0] <= maxSize && size[1] <= maxSize;
}

//------------------------------------------------------------------------------
// Set the modelview transform and load the texture
void vtkOpenGLImageSliceMapper::Render(vtkRenderer* ren, vtkImageSlice* prop)
{
  vtkOpenGLClearErrorMacro();

  vtkOpenGLRenderWindow* renWin = vtkOpenGLRenderWindow::SafeDownCast(ren->GetRenderWindow());

  // update the input information
  vtkImageData* input = this->GetInput();
  input->GetSpacing(this->DataSpacing);
  vtkMatrix3x3::DeepCopy(this->DataDirection, input->GetDirectionMatrix());
  input->GetOrigin(this->DataOrigin);
  vtkInformation* inputInfo = this->GetInputInformation(0, 0);
  inputInfo->Get(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(), this->DataWholeExtent);

  vtkMatrix4x4* matrix = this->GetDataToWorldMatrix();
  this->PolyDataActor->SetUserMatrix(matrix);
  this->BackingPolyDataActor->SetUserMatrix(matrix);
  this->BackgroundPolyDataActor->SetUserMatrix(matrix);
  if (prop->GetPropertyKeys())
  {
    this->PolyDataActor->SetPropertyKeys(prop->GetPropertyKeys());
    this->BackingPolyDataActor->SetPropertyKeys(prop->GetPropertyKeys());
    this->BackgroundPolyDataActor->SetPropertyKeys(prop->GetPropertyKeys());
  }

  // and now enable/disable as needed for our render
  //  glDisable(GL_CULL_FACE);
  //  glDisable(GL_COLOR_MATERIAL);

  // do an offset to avoid depth buffer issues
  // this->PolyDataActor->GetMapper()->
  //   SetResolveCoincidentTopology(VTK_RESOLVE_POLYGON_OFFSET);
  // this->PolyDataActor->GetMapper()->
  //   SetRelativeCoincidentTopologyPolygonOffsetParameters(1.0,100);

  // Add all the clipping planes  TODO: really in the mapper
  // int numClipPlanes = this->GetNumberOfClippingPlanes();

  vtkOpenGLState* ostate = renWin->GetState();

  // Whether to write to the depth buffer and color buffer
  ostate->vtkglDepthMask(this->DepthEnable ? GL_TRUE : GL_FALSE); // supported in all
  if (!this->ColorEnable && !this->MatteEnable)
  {
    ostate->vtkglColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE); // supported in all
  }

  // color and lighting related items
  vtkImageProperty* property = prop->GetProperty();
  double opacity = property->GetOpacity();
  double ambient = property->GetAmbient();
  double diffuse = property->GetDiffuse();
  vtkProperty* pdProp = this->PolyDataActor->GetProperty();
  pdProp->SetOpacity(opacity);
  pdProp->SetAmbient(ambient);
  pdProp->SetDiffuse(diffuse);

  // render the backing polygon
  int backing = property->GetBacking();
  double* bcolor = property->GetBackingColor();
  if (backing && (this->MatteEnable || (this->DepthEnable && !this->ColorEnable)))
  {
    // the backing polygon is always opaque
    pdProp = this->BackingPolyDataActor->GetProperty();
    pdProp->SetOpacity(1.0);
    pdProp->SetAmbient(ambient);
    pdProp->SetDiffuse(diffuse);
    pdProp->SetColor(bcolor[0], bcolor[1], bcolor[2]);
    this->RenderPolygon(this->BackingPolyDataActor, this->Points, this->DisplayExtent, ren);
    if (this->Background)
    {
      double bkcolor[4];
      this->GetBackgroundColor(property, bkcolor);
      pdProp = this->BackgroundPolyDataActor->GetProperty();
      pdProp->SetOpacity(1.0);
      pdProp->SetAmbient(ambient);
      pdProp->SetDiffuse(diffuse);
      pdProp->SetColor(bkcolor[0], bkcolor[1], bkcolor[2]);
      this->RenderBackground(this->BackgroundPolyDataActor, this->Points, this->DisplayExtent, ren);
    }
  }

  // render the texture
  if (this->ColorEnable || (!backing && this->DepthEnable))
  {
    this->RecursiveRenderTexturedPolygon(
      ren, property, this->GetInput(), this->DisplayExtent, false);
  }

  // Set the masks back again
  ostate->vtkglDepthMask(GL_TRUE);
  if (!this->ColorEnable && !this->MatteEnable)
  {
    ostate->vtkglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  }

  this->TimeToDraw = 0.0001;

  vtkOpenGLCheckErrorMacro("failed after Render");
}

//------------------------------------------------------------------------------
void vtkOpenGLImageSliceMapper::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}
VTK_ABI_NAMESPACE_END
