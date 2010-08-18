#include <seamless/Geographic>

#include <algorithm>
#include <iterator>
#include <vector>

#include <osg/ClusterCullingCallback>
#include <osg/CoordinateSystemNode>
#include <osg/Math>

#include <osgEarth/Notify>

#include <seamless/QSC>

namespace seamless
{
using namespace std;
using namespace osg;
using namespace osgEarth;

class GeographicOptions : public PatchOptions
{
public:
    GeographicOptions()
    {
    }
    GeographicOptions(string& str)
        : PatchOptions(str)
    {
    }
    GeographicOptions(const GeographicOptions& rhs,
                     const CopyOp& copyop = CopyOp::SHALLOW_COPY)
        : PatchOptions(rhs, copyop),
          _tileKey(static_cast<TileKey*>(copyop(rhs._tileKey.get())))
    {

    }
    META_Object(seamless, GeographicOptions);
    void setTileKey(TileKey* key) { _tileKey = key; }
    TileKey* getTileKey() const { return _tileKey.get(); }
protected:
    ref_ptr<TileKey> _tileKey;
};

// Hard-wire the patch resolution and screen-space polygon size.
Geographic::Geographic(Map* map)
    : PatchSet(64, new GeographicOptions), _map(map), _profile(new QscProfile),
      _eModel(new EllipsoidModel)
{
    setPrecisionFactor(8);
    const MapLayerList& heightList = _map->getHeightFieldMapLayers();
    {
        int maxLevel = 0;
        Threading::ScopedReadLock lock(_map->getMapDataMutex());
        for (MapLayerList::const_iterator itr = heightList.begin(),
                 end = heightList.end();
             itr != end;
             ++itr)
            if ((*itr)->maxLevel().isSet()
                && (*itr)->maxLevel().get() > maxLevel)
                maxLevel = (*itr)->maxLevel().get();
        if (maxLevel > 0)
            setMaxLevel(maxLevel);
    }
}

Geographic::Geographic(const Geographic& rhs, const osg::CopyOp& copyop)
    : PatchSet(rhs, copyop), _map(static_cast<Map*>(copyop(rhs._map.get()))),
      _profile(static_cast<QscProfile*>(copyop(rhs._profile.get()))),
      _eModel(static_cast<EllipsoidModel*>(copyop(rhs._eModel.get())))
                                  
{
}

Node* Geographic::createPatchSetGraph(const std::string& filename)
{
    CoordinateSystemNode* csn = new CoordinateSystemNode;
    // Should these values come from the map profile?
    csn->setCoordinateSystem("EPSG:4326");
    csn->setFormat("WKT");
    csn->setEllipsoidModel(_eModel.get());
    for (int face = 0; face < 6; ++face)
    {
        double x = 0.0, y = 0.0;
        qsc::faceToCube(x, y, face);
        GeographicOptions* goptions = static_cast<GeographicOptions*>(
            osg::clone(getPatchOptionsPrototype()));
        goptions->setPatchSet(this);
        goptions->setTileKey(_profile->createTileKey(x, y, 0));
        Node* node = createPatchGroup("foobar.tengpatch", goptions);
        csn->addChild(node);
    }
    return csn;
}

// Create the geometry for a patch
MatrixTransform* Geographic::createPatchAux(const TileKey* key,
                                            TileKeyList& keyList)
{
    typedef vector<ref_ptr<GeoHeightField> > GeoHeightFieldList;
    Patch* patch = new Patch;
    patch->setPatchSet(this);
    const GeoExtent& patchExtent = key->getGeoExtent();
    double centx, centy;
    patchExtent.getCentroid(centx, centy);
    Vec3d patchCenter = toModel(centx, centy, 0);
    Matrixd patchMat = Matrixd::translate(patchCenter);
    GeoHeightFieldList hfs;
    for (TileKeyList::iterator itr = keyList.begin(), end = keyList.end();
         itr != end;
        ++itr)
    {
        const TileKey* key = itr->get();
        HeightField* hf = _map->createHeightField(key, true, INTERP_BILINEAR);
        if  (!hf)
            hf = key->getProfile()->getVerticalSRS()
                ->createReferenceHeightField(key->getGeoExtent(),
                                             _resolution + 1, _resolution + 1);
        hfs.push_back(new GeoHeightField(hf, key->getGeoExtent(), 0));
    }
    const SpatialReference* srs = key->getProfile()->getSRS();
    const SpatialReference* mapSrs = _map->getProfile()->getSRS();
    // Populate cell
    ref_ptr<Patch::Data> data = new Patch::Data;
    int patchDim = _resolution + 1;
    Vec3Array* verts = new Vec3Array(patchDim * patchDim);
    Vec3Array* normals = new Vec3Array(patchDim * patchDim);
    Vec2Array* texCoords = new Vec2Array(patchDim * patchDim);
    double xInc = (patchExtent.xMax() - patchExtent.xMin()) / _resolution;
    double yInc = (patchExtent.yMax() - patchExtent.yMin()) / _resolution;
    for (int j = 0; j < patchDim; ++j)
    {
        for (int i = 0; i < patchDim; i++)
        {
            Vec2d cubeCoord(patchExtent.xMin() + i * xInc,
                            patchExtent.yMin() + j * yInc);
            double lon, lat;
            srs->transform(cubeCoord.x(), cubeCoord.y(), mapSrs, lon, lat);
            bool found = false;
            float elevation;
            for (GeoHeightFieldList::iterator itr = hfs.begin(),
                     end = hfs.end();
                 itr != end;
                ++itr)
            {
                if ((*itr)->getElevation(mapSrs, lon, lat, INTERP_BILINEAR, 0,
                                         elevation))
                {
                    found = true;
                    break;
                }
            }
            // Into ec coordinates
            if (!found)
            {
                OE_WARN << "Couldn't find height sample for cube coordinates "
                        << cubeCoord.x() << ", " << cubeCoord.y()
                        << " (lon lat " << lon << ", " << lat << ")\n";
                continue;
            }
            Vec3d coord;
            _eModel->convertLatLongHeightToXYZ(
                DegreesToRadians(lat), DegreesToRadians(lon), elevation,
                coord.x(), coord.y(), coord.z());
            (*verts)[j * patchDim + i] = coord - patchCenter;
            if (fabs((*verts)[j * patchDim + i].z()) > 6000000)
            {
                OE_WARN << "found huge coordinate.\n";
            }
            (*texCoords)[j * patchDim +i]
                = Vec2(i / static_cast<float>(_resolution),
                       j / static_cast<float>(_resolution));
        }
    }
    // Normals. Average the normals of the triangles around the sample
    // point. We're not following the actual tessallation of the grid.
    for (int j = 0; j < patchDim; ++j)
    {
        for (int i = 0; i < patchDim; i++)
        {
            const Vec3& pt = (*verts)[j * patchDim + i];
            // A cross of points.
            Vec3 delta[4];      // Initialized to zero vectors
            for (int k = 0; k < 2; ++k)
            {
                int gridx = i + 2 * k - 1;
                if (gridx < patchDim && gridx >= 0)
                    delta[2 * k] = (*verts)[j * patchDim + gridx] - pt;
            }
            for (int k = 0; k < 2; ++k)
            {
                int gridy = j + 2 * k - 1;
                if (gridy < patchDim && gridy >= 0)
                    delta[2 * k + 1] = (*verts)[gridy * patchDim + i] - pt;
            }
            Vec3 normal;
            for (int k = 1; k <= 4; ++k)
            {
                int v1 = k - 1, v2 = k % 4;
                // If One or both of the deltas are 0, then the cross
                // product is 0 and won't contribute to the average.
                normal += delta[v1] ^ delta[v2];
            }
            normal.normalize();
            (*normals)[j * patchDim + i] = normal;
        }
    }
    // Construct the patch and its transform.
    data->vertexData.array = verts;
    data->vertexData.binding = Geometry::BIND_PER_VERTEX;
    data->normalData.array = normals;
    data->normalData.binding = Geometry::BIND_PER_VERTEX;
    Vec4Array* colors = new Vec4Array(1);
    (*colors)[0] = Vec4(1.0, 1.0, 1.0, 1.0);
    data->colorData.array = colors;
    data->colorData.binding = Geometry::BIND_OVERALL;
    data->texCoordList
        .push_back(Geometry::ArrayData(texCoords, Geometry::BIND_PER_VERTEX));
    patch->setData(data);
    MatrixTransform* result = new MatrixTransform;
    result->addChild(patch);
    result->setMatrix(patchMat);
    return result;
}

        
Node* Geographic::createPatch(const std::string& filename,
                              PatchOptions* poptions)
{
    GeographicOptions* goptions = static_cast<GeographicOptions*>(poptions);
    const TileKey* patchKey = goptions->getTileKey();
    int face = QscProfile::getFace(patchKey);
    TileKeyList mapKeys;
    // Split up patch keys that cross the Date Line. The only patches
    // that do that are the top-level patches for the equatorial patch
    // with center at (-180, 0) and the poles.
    const GeoExtent& keyExtent = patchKey->getGeoExtent();
    if ((face == 2 || face == 4 || face == 5)
        && keyExtent.xMax() - keyExtent.xMin() > .5)
    {
        for (int child = 0; child < 4; ++child)
        {
            TileKeyList subMapKeys;
            ref_ptr<TileKey> subCubeKey = patchKey->createSubkey(child);
            _map->getProfile()->getIntersectingTiles(subCubeKey.get(),
                                                     subMapKeys);
            copy(subMapKeys.begin(), subMapKeys.end(), back_inserter(mapKeys));
        }
    }
    else
    {
        _map->getProfile()->getIntersectingTiles(patchKey, mapKeys);
    }
    MatrixTransform* transform = createPatchAux(patchKey, mapKeys);
#if 0
    ref_ptr<GeoImage> gimage;
    if (!_map->getImageMapLayers().empty())
        gimage = _map->getImageMapLayers()[0]->createImage(key.get());

    int patchDim = _resolution + 1;
    hf = resampleHeightField(hf, patchDim);
    Vec3Array* verts = new Vec3Array(patchDim * patchDim);
    Vec3Array* normals = new Vec3Array(patchDim * patchDim);
    Vec2f minCoord(xMin - centerX, yMin - centerY);
    float xInt = hf->getXInterval(), yInt = hf->getYInterval();

    if (gimage)
    {
        Texture2D* tex = new Texture2D();
        tex->setImage(gimage->getImage());
        tex->setWrap(Texture::WRAP_S, Texture::CLAMP_TO_EDGE);
        tex->setWrap(Texture::WRAP_T, Texture::CLAMP_TO_EDGE);
        tex->setFilter(Texture::MIN_FILTER, Texture::LINEAR_MIPMAP_LINEAR);
        tex->setFilter(Texture::MAG_FILTER, Texture::LINEAR);
        StateSet* ss = patch->getOrCreateStateSet();
        ss->setTextureAttributeAndModes(0, tex, StateAttribute::ON);
    }
#endif
    return transform;
}

namespace
{
ClusterCullingCallback*
createClusterCullingCallback(const Matrixd& transform, const Patch* patch,
                             const EllipsoidModel* et)
{
    //This code is a very slightly modified version of the
    //DestinationTile::createClusterCullingCallback in
    //VirtualPlanetBuilder.
    double globe_radius =  et->getRadiusPolar();
    Vec3 center_position(transform.getTrans());
    Vec3 center_normal(center_position);
    center_normal.normalize();
    
    unsigned int r,c;
    
    // populate the vertex/normal/texcoord arrays from the grid.

    float min_dot_product = 1.0f;
    float max_cluster_culling_height = 0.0f;
    float max_cluster_culling_radius = 0.0f;
    const Vec3Array* verts = static_cast<const Vec3Array*>(
        patch->getData()->vertexData.array.get());
    for (Vec3Array::const_iterator itr = verts->begin(), end = verts->end();
         itr != end;
         ++itr)
    {
        Vec3d dv = *itr;
        Vec3d v = dv + center_position;
        double lat, lon, height;

        et->convertXYZToLatLongHeight(v.x(), v.y(), v.z(),
                                      lat, lon, height);

        double d = sqrt(dv.x()*dv.x() + dv.y()*dv.y() + dv.z()*dv.z());
        double theta = acos(globe_radius / (globe_radius + fabs(height)));
        double phi = 2.0 * asin (d*0.5 / globe_radius); // d/globe_radius;
        double beta = theta + phi;
        double sb = sin(beta);
        double cb = cos(beta);
        double cutoff = osg::PI_2 - 0.1;
            
        //log(osg::INFO,"theta="<<theta<<"\tphi="<<phi<<" beta "<<beta);
        if (phi<cutoff && beta<cutoff)
        {
            float local_dot_product = -sb;
            float local_m = globe_radius*( 1.0/ cb - 1.0);
            float local_radius = static_cast<float>(globe_radius * sb / cb); // beta*globe_radius;
            min_dot_product = osg::minimum(min_dot_product, local_dot_product);
            max_cluster_culling_height = osg::maximum(max_cluster_culling_height,local_m);      
            max_cluster_culling_radius = osg::maximum(max_cluster_culling_radius,local_radius);
        }
        else
        {
            //log(osg::INFO,"Turning off cluster culling for wrap around tile.");
            return 0;
        }
    }

    osg::ClusterCullingCallback* ccc = new osg::ClusterCullingCallback;

    ccc->set(center_position + center_normal*max_cluster_culling_height ,
             center_normal, 
             min_dot_product,
             max_cluster_culling_radius);

    return ccc;
}
}

Node* Geographic::createPatchGroup(const string& filename,
                                   PatchOptions* poptions)
{
    Node* result = PatchSet::createPatchGroup(filename, poptions);
    PatchGroup* pgroup = dynamic_cast<PatchGroup*>(result);
    // Make a cluster culling callback
    MatrixTransform* transform
        = dynamic_cast<MatrixTransform*>(pgroup->getChild(0));
    Patch* patch = dynamic_cast<Patch*>(transform->getChild(0));
    ClusterCullingCallback* ccc
        = createClusterCullingCallback(transform->getMatrix(), patch,
                                       _eModel.get());
    pgroup->setCullCallback(ccc);
    return pgroup;
}

Vec3d Geographic::toModel(double cubeX, double cubeY, double elevation)
{
    double faceX = cubeX, faceY = cubeY;
    int face;
    qsc::cubeToFace(faceX, faceY, face);
    double lat_deg, lon_deg;
    qsc::faceCoordsToLatLon(faceX, faceY, face, lat_deg, lon_deg);
    Vec3d result;
    _eModel->convertLatLongHeightToXYZ(
        DegreesToRadians(lat_deg), DegreesToRadians(lon_deg), elevation,
        result.x(), result.y(), result.z());
    return result;
}

Node* Geographic::createChild(const PatchOptions* parentOptions, int childNum)
{
    const GeographicOptions* parentgopt
        = static_cast<const GeographicOptions*>(parentOptions);
    GeographicOptions* goptions = osg::clone(parentgopt);
    goptions->setPatchLevel(parentgopt->getPatchLevel() + 1);
    goptions->setTileKey(parentgopt->getTileKey()->createSubkey(childNum));
    return createPatchGroup("foobies.tengpatch", goptions);
    
}
}
