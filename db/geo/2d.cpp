// geo2d.cpp

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "../namespace-inl.h"
#include "../jsobj.h"
#include "../index.h"
#include "../../util/unittest.h"
#include "../commands.h"
#include "../pdfile.h"
#include "../btree.h"
#include "../curop-inl.h"
#include "../matcher.h"

#include "core.h"

namespace mongo {

#if 0
# define GEODEBUGGING
# define GEODEBUG(x) cout << x << endl;
# define GEODEBUGPRINT(x) PRINT(x)
    inline void PREFIXDEBUG(GeoHash prefix, const GeoConvert* g) {
        if (!prefix.constrains()) {
            cout << "\t empty prefix" << endl;
            return ;
        }

        Point ll (g, prefix); // lower left
        prefix.move(1,1);
        Point tr (g, prefix); // top right

        Point center ( (ll._x+tr._x)/2, (ll._y+tr._y)/2 );
        double radius = fabs(ll._x - tr._x) / 2;

        cout << "\t ll: " << ll.toString() << " tr: " << tr.toString()
             << " center: " << center.toString() << " radius: " << radius << endl;

    }
#else
# define GEODEBUG(x)
# define GEODEBUGPRINT(x)
# define PREFIXDEBUG(x, y)
#endif

    const double EARTH_RADIUS_KM = 6371;
    const double EARTH_RADIUS_MILES = EARTH_RADIUS_KM * 0.621371192;

    enum GeoDistType {
        GEO_PLAIN,
        GEO_SPHERE
    };

    inline double computeXScanDistance(double y, double maxDistDegrees) {
        // TODO: this overestimates for large madDistDegrees far from the equator
        return maxDistDegrees / min(cos(deg2rad(min(+89.0, y + maxDistDegrees))),
                                    cos(deg2rad(max(-89.0, y - maxDistDegrees))));
    }

    GeoBitSets geoBitSets;

    const string GEO2DNAME = "2d";

    class Geo2dType : public IndexType , public GeoConvert {
    public:
        Geo2dType( const IndexPlugin * plugin , const IndexSpec* spec )
            : IndexType( plugin , spec ) {

            BSONObjBuilder orderBuilder;

            BSONObjIterator i( spec->keyPattern );
            while ( i.more() ) {
                BSONElement e = i.next();
                if ( e.type() == String && GEO2DNAME == e.valuestr() ) {
                    uassert( 13022 , "can't have 2 geo field" , _geo.size() == 0 );
                    uassert( 13023 , "2d has to be first in index" , _other.size() == 0 );
                    _geo = e.fieldName();
                }
                else {
                    _other.push_back( e.fieldName() );
                }
                orderBuilder.append( "" , 1 );
            }

            uassert( 13024 , "no geo field specified" , _geo.size() );

            double bits =  _configval( spec , "bits" , 26 ); // for lat/long, ~ 1ft

            uassert( 13028 , "bits in geo index must be between 1 and 32" , bits > 0 && bits <= 32 );

            _bits = (unsigned) bits;

            _max = _configval( spec , "max" , 180.0 );
            _min = _configval( spec , "min" , -180.0 );

            double numBuckets = (1024 * 1024 * 1024 * 4.0);

            _scaling = numBuckets / ( _max - _min );

            _order = orderBuilder.obj();

            GeoHash a(0, 0, _bits);
            GeoHash b = a;
            b.move(1, 1);

            _error = distance(a, b);
            // Error in radians
            _errorSphere = deg2rad( _error );
        }

        double _configval( const IndexSpec* spec , const string& name , double def ) {
            BSONElement e = spec->info[name];
            if ( e.isNumber() ) {
                return e.numberDouble();
            }
            return def;
        }

        ~Geo2dType() {

        }

        virtual BSONObj fixKey( const BSONObj& in ) {
            if ( in.firstElement().type() == BinData )
                return in;

            BSONObjBuilder b(in.objsize()+16);

            if ( in.firstElement().isABSONObj() )
                _hash( in.firstElement().embeddedObject() ).append( b , "" );
            else if ( in.firstElement().type() == String )
                GeoHash( in.firstElement().valuestr() ).append( b , "" );
            else if ( in.firstElement().type() == RegEx )
                GeoHash( in.firstElement().regex() ).append( b , "" );
            else
                return in;

            BSONObjIterator i(in);
            i.next();
            while ( i.more() )
                b.append( i.next() );
            return b.obj();
        }

        /** Finds the key objects to put in an index */
        virtual void getKeys( const BSONObj& obj, BSONObjSetDefaultOrder& keys ) const {
            getKeys( obj, &keys, NULL );
        }

        /** Finds all locations in a geo-indexed object */
        // TODO:  Can we just return references to the locs, if they won't change?
        void getKeys( const BSONObj& obj, vector< BSONObj >& locs ) const {
            getKeys( obj, NULL, &locs );
        }

        /** Finds the key objects and/or locations for a geo-indexed object */
        void getKeys( const BSONObj &obj, BSONObjSetDefaultOrder* keys, vector< BSONObj >* locs ) const {

            BSONElementSet bSet;
            // Get all the nested location fields, but don't return individual elements from
            // the last array, if it exists.
            obj.getFieldsDotted(_geo.c_str(), bSet, false);

            if( bSet.empty() )
                return;

            for( BSONElementSet::iterator setI = bSet.begin(); setI != bSet.end(); ++setI ) {

                BSONElement geo = *setI;

                GEODEBUG( "Element " << geo << " found for query " << _geo.c_str() );

                if ( geo.eoo() || ! geo.isABSONObj() )
                    continue;

                //
                // Grammar for location lookup:
                // locs ::= [loc,loc,...,loc]|{<k>:loc,<k>:loc}|loc
                // loc  ::= { <k1> : #, <k2> : # }|[#, #]|{}
                //
                // Empty locations are ignored, preserving single-location semantics
                //

                BSONObj embed = geo.embeddedObject();
                if ( embed.isEmpty() )
                    continue;

                // Differentiate between location arrays and locations
                // by seeing if the first element value is a number
                bool singleElement = embed.firstElement().isNumber();

                BSONObjIterator oi(embed);

                while( oi.more() ) {

                    BSONObj locObj;

                    if( singleElement ) locObj = embed;
                    else {
                        BSONElement locElement = oi.next();

                        uassert( 13654, str::stream() << "location object expected, location array not in correct format",
                                 locElement.isABSONObj() );

                        locObj = locElement.embeddedObject();

                        if( locObj.isEmpty() )
                            continue;
                    }

                    BSONObjBuilder b(64);

                    // Remember the actual location object if needed
                    if( locs )
                        locs->push_back( locObj );

                    // Stop if we don't need to get anything but location objects
                    if( ! keys ) {
                        if( singleElement ) break;
                        else continue;
                    }

                    _hash( locObj ).append( b , "" );

                    // Go through all the other index keys
                    for ( vector<string>::const_iterator i = _other.begin(); i != _other.end(); ++i ) {

                        // Get *all* fields for the index key
                        BSONElementSet eSet;
                        obj.getFieldsDotted( *i, eSet );


                        if ( eSet.size() == 0 )
                            b.appendAs( _spec->missingField(), "" );
                        else if ( eSet.size() == 1 )
                            b.appendAs( *(eSet.begin()), "" );
                        else {

                            // If we have more than one key, store as an array of the objects

                            BSONArrayBuilder aBuilder;

                            for( BSONElementSet::iterator ei = eSet.begin(); ei != eSet.end(); ++ei ) {
                                aBuilder.append( *ei );
                            }

                            BSONArray arr = aBuilder.arr();

                            b.append( "", arr );

                        }

                    }

                    keys->insert( b.obj() );

                    if( singleElement ) break;

                }
            }

        }

        GeoHash _tohash( const BSONElement& e ) const {
            if ( e.isABSONObj() )
                return _hash( e.embeddedObject() );

            return GeoHash( e , _bits );
        }

        GeoHash _hash( const BSONObj& o ) const {
            BSONObjIterator i(o);
            uassert( 13067 , "geo field is empty" , i.more() );
            BSONElement x = i.next();
            uassert( 13068 , "geo field only has 1 element" , i.more() );
            BSONElement y = i.next();

            uassert( 13026 , "geo values have to be numbers: " + o.toString() , x.isNumber() && y.isNumber() );

            return hash( x.number() , y.number() );
        }

        GeoHash hash( const Point& p ) const {
            return hash( p._x, p._y );
        }

        GeoHash hash( double x , double y ) const {
            return GeoHash( _convert(x), _convert(y) , _bits );
        }

        BSONObj _unhash( const GeoHash& h ) const {
            unsigned x , y;
            h.unhash( x , y );
            BSONObjBuilder b;
            b.append( "x" , _unconvert( x ) );
            b.append( "y" , _unconvert( y ) );
            return b.obj();
        }

        unsigned _convert( double in ) const {
            uassert( 13027 , str::stream() << "point not in interval of [ " << _min << ", " << _max << " )", in < _max && in >= _min );
            in -= _min;
            assert( in >= 0 );
            return (unsigned)(in * _scaling);
        }

        double _unconvert( unsigned in ) const {
            double x = in;
            x /= _scaling;
            x += _min;
            return x;
        }

        void unhash( const GeoHash& h , double& x , double& y ) const {
            unsigned a,b;
            h.unhash(a,b);
            x = _unconvert( a );
            y = _unconvert( b );
        }

        double distance( const GeoHash& a , const GeoHash& b ) const {
            double ax,ay,bx,by;
            unhash( a , ax , ay );
            unhash( b , bx , by );

            double dx = bx - ax;
            double dy = by - ay;

            return sqrt( ( dx * dx ) + ( dy * dy ) );
        }

        double sizeDiag( const GeoHash& a ) const {
            GeoHash b = a;
            b.move( 1 , 1 );
            return distance( a , b );
        }

        double sizeEdge( const GeoHash& a ) const {
            double ax,ay,bx,by;
            GeoHash b = a;
            b.move( 1 , 1 );
            unhash( a, ax, ay );
            unhash( b, bx, by );

            // _min and _max are a singularity
            if (bx == _min)
                bx = _max;

            return (fabs(ax-bx));
        }

        const IndexDetails* getDetails() const {
            return _spec->getDetails();
        }

        virtual shared_ptr<Cursor> newCursor( const BSONObj& query , const BSONObj& order , int numWanted ) const;

        virtual IndexSuitability suitability( const BSONObj& query , const BSONObj& order ) const {
            BSONElement e = query.getFieldDotted(_geo.c_str());
            switch ( e.type() ) {
            case Object: {
                BSONObj sub = e.embeddedObject();
                switch ( sub.firstElement().getGtLtOp() ) {
                case BSONObj::opNEAR:
                case BSONObj::opWITHIN:
                    return OPTIMAL;
                default:;
                }
            }
            case Array:
                // Non-geo index data is stored in a non-standard way, cannot use for exact lookups with
                // additional criteria
                if ( query.nFields() > 1 ) return USELESS;
                return HELPFUL;
            default:
                return USELESS;
            }
        }

        string _geo;
        vector<string> _other;

        unsigned _bits;
        double _max;
        double _min;
        double _scaling;

        BSONObj _order;
        double _error;
        double _errorSphere;
    };

    class Box {
    public:

        Box( const Geo2dType * g , const GeoHash& hash )
            : _min( g , hash ) ,
              _max( _min._x + g->sizeEdge( hash ) , _min._y + g->sizeEdge( hash ) ) {
        }

        Box( double x , double y , double size )
            : _min( x , y ) ,
              _max( x + size , y + size ) {
        }

        Box( Point min , Point max )
            : _min( min ) , _max( max ) {
        }

        Box() {}

        string toString() const {
            StringBuilder buf(64);
            buf << _min.toString() << " -->> " << _max.toString();
            return buf.str();
        }

        bool between( double min , double max , double val , double fudge=0) const {
            return val + fudge >= min && val <= max + fudge;
        }

        bool onBoundary( double bound, double val, double fudge = 0 ) const {
            return ( val >= bound - fudge && val <= bound + fudge );
        }

        bool mid( double amin , double amax , double bmin , double bmax , bool min , double& res ) const {
            assert( amin <= amax );
            assert( bmin <= bmax );

            if ( amin < bmin ) {
                if ( amax < bmin )
                    return false;
                res = min ? bmin : amax;
                return true;
            }
            if ( amin > bmax )
                return false;
            res = min ? amin : bmax;
            return true;
        }

        double intersects( const Box& other ) const {

            Point boundMin(0,0);
            Point boundMax(0,0);

            if ( mid( _min._x , _max._x , other._min._x , other._max._x , true , boundMin._x ) == false ||
                    mid( _min._x , _max._x , other._min._x , other._max._x , false , boundMax._x ) == false ||
                    mid( _min._y , _max._y , other._min._y , other._max._y , true , boundMin._y ) == false ||
                    mid( _min._y , _max._y , other._min._y , other._max._y , false , boundMax._y ) == false )
                return 0;

            Box intersection( boundMin , boundMax );

            return intersection.area() / ( ( area() + other.area() ) / 2 );
        }

        double area() const {
            return ( _max._x - _min._x ) * ( _max._y - _min._y );
        }

        double maxDim() const {
            return max( _max._x - _min._x, _max._y - _min._y );
        }

        Point center() const {
            return Point( ( _min._x + _max._x ) / 2 ,
                          ( _min._y + _max._y ) / 2 );
        }

        bool onBoundary( Point p, double fudge = 0 ) {
            return onBoundary( _min._x, p._x, fudge ) ||
                   onBoundary( _max._x, p._x, fudge ) ||
                   onBoundary( _min._y, p._y, fudge ) ||
                   onBoundary( _max._y, p._y, fudge );
        }

        bool inside( Point p , double fudge = 0 ) {
            bool res = inside( p._x , p._y , fudge );
            //cout << "is : " << p.toString() << " in " << toString() << " = " << res << endl;
            return res;
        }

        bool inside( double x , double y , double fudge = 0 ) {
            return
                between( _min._x , _max._x  , x , fudge ) &&
                between( _min._y , _max._y  , y , fudge );
        }

        bool contains(const Box& other, double fudge=0) {
            return inside(other._min, fudge) && inside(other._max, fudge);
        }

        Point _min;
        Point _max;
    };


    class Polygon {
    public:

        Polygon( void ) : _centroidCalculated( false ) {}

        Polygon( vector<Point> points ) : _centroidCalculated( false ),
            _points( points ) { }

        void add( Point p ) {
            _centroidCalculated = false;
            _points.push_back( p );
        }

        int size( void ) const {
            return _points.size();
        }

        /**
         * Determine if the point supplied is contained by the current polygon.
         *
         * The algorithm uses a ray casting method.
         */
        bool contains( const Point& p ) const {
            return contains( p, 0 ) > 0;
        }

        int contains( const Point &p, double fudge ) const {

            Box fudgeBox( Point( p._x - fudge, p._y - fudge ), Point( p._x + fudge, p._y + fudge ) );

            int counter = 0;
            Point p1 = _points[0];
            for ( int i = 1; i <= size(); i++ ) {
                Point p2 = _points[i % size()];

                GEODEBUG( "Doing intersection check of " << fudgeBox << " with seg " << p1 << " to " << p2 );

                // We need to check whether or not this segment intersects our error box
                if( fudge > 0 &&
                        // Points not too far below box
                        fudgeBox._min._y <= std::max( p1._y, p2._y ) &&
                        // Points not too far above box
                        fudgeBox._max._y >= std::min( p1._y, p2._y ) &&
                        // Points not too far to left of box
                        fudgeBox._min._x <= std::max( p1._x, p2._x ) &&
                        // Points not too far to right of box
                        fudgeBox._max._x >= std::min( p1._x, p2._x ) ) {

                    GEODEBUG( "Doing detailed check" );

                    // If our box contains one or more of these points, we need to do an exact check.
                    if( fudgeBox.inside(p1) ) {
                        GEODEBUG( "Point 1 inside" );
                        return 0;
                    }
                    if( fudgeBox.inside(p2) ) {
                        GEODEBUG( "Point 2 inside" );
                        return 0;
                    }

                    // Do intersection check for vertical sides
                    if ( p1._y != p2._y ) {

                        double invSlope = ( p2._x - p1._x ) / ( p2._y - p1._y );

                        double xintersT = ( fudgeBox._max._y - p1._y ) * invSlope + p1._x;
                        if( fudgeBox._min._x <= xintersT && fudgeBox._max._x >= xintersT ) {
                            GEODEBUG( "Top intersection @ " << xintersT );
                            return 0;
                        }

                        double xintersB = ( fudgeBox._min._y - p1._y ) * invSlope + p1._x;
                        if( fudgeBox._min._x <= xintersB && fudgeBox._max._x >= xintersB ) {
                            GEODEBUG( "Bottom intersection @ " << xintersB );
                            return 0;
                        }

                    }

                    // Do intersection check for horizontal sides
                    if( p1._x != p2._x ) {

                        double slope = ( p2._y - p1._y ) / ( p2._x - p1._x );

                        double yintersR = ( p1._x - fudgeBox._max._x ) * slope + p1._y;
                        if( fudgeBox._min._y <= yintersR && fudgeBox._max._y >= yintersR ) {
                            GEODEBUG( "Right intersection @ " << yintersR );
                            return 0;
                        }

                        double yintersL = ( p1._x - fudgeBox._min._x ) * slope + p1._y;
                        if( fudgeBox._min._y <= yintersL && fudgeBox._max._y >= yintersL ) {
                            GEODEBUG( "Left intersection @ " << yintersL );
                            return 0;
                        }

                    }

                }

                // Normal intersection test.
                // TODO: Invert these for clearer logic?
                if ( p._y > std::min( p1._y, p2._y ) ) {
                    if ( p._y <= std::max( p1._y, p2._y ) ) {
                        if ( p._x <= std::max( p1._x, p2._x ) ) {
                            if ( p1._y != p2._y ) {
                                double xinters = (p._y-p1._y)*(p2._x-p1._x)/(p2._y-p1._y)+p1._x;
                                if ( p1._x == p2._x || p._x <= xinters ) {
                                    counter++;
                                }
                            }
                        }
                    }
                }

                p1 = p2;
            }

            if ( counter % 2 == 0 ) {
                return -1;
            }
            else {
                return 1;
            }
        }

        /**
         * Calculate the centroid, or center of mass of the polygon object.
         */
        Point centroid( void ) {

            /* Centroid is cached, it won't change betwen points */
            if ( _centroidCalculated ) {
                return _centroid;
            }

            Point cent;
            double signedArea = 0.0;
            double area = 0.0;  // Partial signed area

            /// For all vertices except last
            int i = 0;
            for ( i = 0; i < size() - 1; ++i ) {
                area = _points[i]._x * _points[i+1]._y - _points[i+1]._x * _points[i]._y ;
                signedArea += area;
                cent._x += ( _points[i]._x + _points[i+1]._x ) * area;
                cent._y += ( _points[i]._y + _points[i+1]._y ) * area;
            }

            // Do last vertex
            area = _points[i]._x * _points[0]._y - _points[0]._x * _points[i]._y;
            cent._x += ( _points[i]._x + _points[0]._x ) * area;
            cent._y += ( _points[i]._y + _points[0]._y ) * area;
            signedArea += area;
            signedArea *= 0.5;
            cent._x /= ( 6 * signedArea );
            cent._y /= ( 6 * signedArea );

            _centroidCalculated = true;
            _centroid = cent;

            return cent;
        }

        Box bounds( void ) {

            // TODO: Cache this

            _bounds._max = _points[0];
            _bounds._min = _points[0];

            for ( int i = 1; i < size(); i++ ) {

                _bounds._max._x = max( _bounds._max._x, _points[i]._x );
                _bounds._max._y = max( _bounds._max._y, _points[i]._y );
                _bounds._min._x = min( _bounds._min._x, _points[i]._x );
                _bounds._min._y = min( _bounds._min._y, _points[i]._y );

            }

            return _bounds;

        }

    private:

        bool _centroidCalculated;
        Point _centroid;

        Box _bounds;

        vector<Point> _points;
    };

    class Geo2dPlugin : public IndexPlugin {
    public:
        Geo2dPlugin() : IndexPlugin( GEO2DNAME ) {
        }

        virtual IndexType* generate( const IndexSpec* spec ) const {
            return new Geo2dType( this , spec );
        }
    } geo2dplugin;

    struct GeoUnitTest : public UnitTest {

        int round( double d ) {
            return (int)(.5+(d*1000));
        }

#define GEOHEQ(a,b) if ( a.toString() != b ){ cout << "[" << a.toString() << "] != [" << b << "]" << endl; assert( a == GeoHash(b) ); }

        void run() {
            assert( ! GeoHash::isBitSet( 0 , 0 ) );
            assert( ! GeoHash::isBitSet( 0 , 31 ) );
            assert( GeoHash::isBitSet( 1 , 31 ) );

            IndexSpec i( BSON( "loc" << "2d" ) );
            Geo2dType g( &geo2dplugin , &i );
            {
                double x = 73.01212;
                double y = 41.352964;
                BSONObj in = BSON( "x" << x << "y" << y );
                GeoHash h = g._hash( in );
                BSONObj out = g._unhash( h );
                assert( round(x) == round( out["x"].number() ) );
                assert( round(y) == round( out["y"].number() ) );
                assert( round( in["x"].number() ) == round( out["x"].number() ) );
                assert( round( in["y"].number() ) == round( out["y"].number() ) );
            }

            {
                double x = -73.01212;
                double y = 41.352964;
                BSONObj in = BSON( "x" << x << "y" << y );
                GeoHash h = g._hash( in );
                BSONObj out = g._unhash( h );
                assert( round(x) == round( out["x"].number() ) );
                assert( round(y) == round( out["y"].number() ) );
                assert( round( in["x"].number() ) == round( out["x"].number() ) );
                assert( round( in["y"].number() ) == round( out["y"].number() ) );
            }

            {
                GeoHash h( "0000" );
                h.move( 0 , 1 );
                GEOHEQ( h , "0001" );
                h.move( 0 , -1 );
                GEOHEQ( h , "0000" );

                h.init( "0001" );
                h.move( 0 , 1 );
                GEOHEQ( h , "0100" );
                h.move( 0 , -1 );
                GEOHEQ( h , "0001" );


                h.init( "0000" );
                h.move( 1 , 0 );
                GEOHEQ( h , "0010" );
            }

            {
                Box b( 5 , 5 , 2 );
                assert( "(5,5) -->> (7,7)" == b.toString() );
            }

            {
                GeoHash a = g.hash( 1 , 1 );
                GeoHash b = g.hash( 4 , 5 );
                assert( 5 == (int)(g.distance( a , b ) ) );
                a = g.hash( 50 , 50 );
                b = g.hash( 42 , 44 );
                assert( round(10) == round(g.distance( a , b )) );
            }

            {
                GeoHash x("0000");
                assert( 0 == x.getHash() );
                x.init( 0 , 1 , 32 );
                GEOHEQ( x , "0000000000000000000000000000000000000000000000000000000000000001" )

                assert( GeoHash( "1100").hasPrefix( GeoHash( "11" ) ) );
                assert( ! GeoHash( "1000").hasPrefix( GeoHash( "11" ) ) );
            }

            {
                GeoHash x("1010");
                GEOHEQ( x , "1010" );
                GeoHash y = x + "01";
                GEOHEQ( y , "101001" );
            }

            {

                GeoHash a = g.hash( 5 , 5 );
                GeoHash b = g.hash( 5 , 7 );
                GeoHash c = g.hash( 100 , 100 );
                /*
                cout << "a: " << a << endl;
                cout << "b: " << b << endl;
                cout << "c: " << c << endl;

                cout << "a: " << a.toStringHex1() << endl;
                cout << "b: " << b.toStringHex1() << endl;
                cout << "c: " << c.toStringHex1() << endl;
                */
                BSONObj oa = a.wrap();
                BSONObj ob = b.wrap();
                BSONObj oc = c.wrap();
                /*
                cout << "a: " << oa.hexDump() << endl;
                cout << "b: " << ob.hexDump() << endl;
                cout << "c: " << oc.hexDump() << endl;
                */
                assert( oa.woCompare( ob ) < 0 );
                assert( oa.woCompare( oc ) < 0 );

            }

            {
                GeoHash x( "000000" );
                x.move( -1 , 0 );
                GEOHEQ( x , "101010" );
                x.move( 1 , -1 );
                GEOHEQ( x , "010101" );
                x.move( 0 , 1 );
                GEOHEQ( x , "000000" );
            }

            {
                GeoHash prefix( "110011000000" );
                GeoHash entry(  "1100110000011100000111000001110000011100000111000001000000000000" );
                assert( ! entry.hasPrefix( prefix ) );

                entry = GeoHash("1100110000001100000111000001110000011100000111000001000000000000");
                assert( entry.toString().find( prefix.toString() ) == 0 );
                assert( entry.hasPrefix( GeoHash( "1100" ) ) );
                assert( entry.hasPrefix( prefix ) );
            }

            {
                GeoHash a = g.hash( 50 , 50 );
                GeoHash b = g.hash( 48 , 54 );
                assert( round( 4.47214 ) == round( g.distance( a , b ) ) );
            }


            {
                Box b( Point( 29.762283 , -95.364271 ) , Point( 29.764283000000002 , -95.36227099999999 ) );
                assert( b.inside( 29.763 , -95.363 ) );
                assert( ! b.inside( 32.9570255 , -96.1082497 ) );
                assert( ! b.inside( 32.9570255 , -96.1082497 , .01 ) );
            }

            {
                GeoHash a( "11001111" );
                assert( GeoHash( "11" ) == a.commonPrefix( GeoHash("11") ) );
                assert( GeoHash( "11" ) == a.commonPrefix( GeoHash("11110000") ) );
            }

            {
                int N = 10000;
                {
                    Timer t;
                    for ( int i=0; i<N; i++ ) {
                        unsigned x = (unsigned)rand();
                        unsigned y = (unsigned)rand();
                        GeoHash h( x , y );
                        unsigned a,b;
                        h.unhash_slow( a,b );
                        assert( a == x );
                        assert( b == y );
                    }
                    //cout << "slow: " << t.millis() << endl;
                }

                {
                    Timer t;
                    for ( int i=0; i<N; i++ ) {
                        unsigned x = (unsigned)rand();
                        unsigned y = (unsigned)rand();
                        GeoHash h( x , y );
                        unsigned a,b;
                        h.unhash_fast( a,b );
                        assert( a == x );
                        assert( b == y );
                    }
                    //cout << "fast: " << t.millis() << endl;
                }

            }

            {
                // see http://en.wikipedia.org/wiki/Great-circle_distance#Worked_example

                {
                    Point BNA (-86.67, 36.12);
                    Point LAX (-118.40, 33.94);

                    double dist1 = spheredist_deg(BNA, LAX);
                    double dist2 = spheredist_deg(LAX, BNA);

                    // target is 0.45306
                    assert( 0.45305 <= dist1 && dist1 <= 0.45307 );
                    assert( 0.45305 <= dist2 && dist2 <= 0.45307 );
                }
                {
                    Point BNA (-1.5127, 0.6304);
                    Point LAX (-2.0665, 0.5924);

                    double dist1 = spheredist_rad(BNA, LAX);
                    double dist2 = spheredist_rad(LAX, BNA);

                    // target is 0.45306
                    assert( 0.45305 <= dist1 && dist1 <= 0.45307 );
                    assert( 0.45305 <= dist2 && dist2 <= 0.45307 );
                }
                {
                    Point JFK (-73.77694444, 40.63861111 );
                    Point LAX (-118.40, 33.94);

                    double dist = spheredist_deg(JFK, LAX) * EARTH_RADIUS_MILES;
                    assert( dist > 2469 && dist < 2470 );
                }

                {
                    Point BNA (-86.67, 36.12);
                    Point LAX (-118.40, 33.94);
                    Point JFK (-73.77694444, 40.63861111 );
                    assert( spheredist_deg(BNA, BNA) < 1e-6);
                    assert( spheredist_deg(LAX, LAX) < 1e-6);
                    assert( spheredist_deg(JFK, JFK) < 1e-6);

                    Point zero (0, 0);
                    Point antizero (0,-180);

                    // these were known to cause NaN
                    assert( spheredist_deg(zero, zero) < 1e-6);
                    assert( fabs(M_PI-spheredist_deg(zero, antizero)) < 1e-6);
                    assert( fabs(M_PI-spheredist_deg(antizero, zero)) < 1e-6);
                }
            }
        }
    } geoUnitTest;

    class GeoHopper;

    class GeoPoint {
    public:
        GeoPoint() {
        }

        //// Distance not used ////

        GeoPoint( const KeyNode& node )
            : _key( node.key ) , _loc( node.recordLoc ) , _o( node.recordLoc.obj() ) , _exactDistance( -1 ), _exactWithin( false ) {
        }

        //// Immediate initialization of exact distance ////

        GeoPoint( const KeyNode& node , double exactDistance, bool exactWithin )
            : _key( node.key ) , _loc( node.recordLoc ) , _o( node.recordLoc.obj() ), _exactDistance( exactDistance ), _exactWithin( exactWithin ) {
        }

        bool operator<( const GeoPoint& other ) const {
            return _exactDistance < other._exactDistance;
        }

        bool isEmpty() const {
            return _o.isEmpty();
        }

        string toString() const {
            return str::stream() << "Point from " << _o.toString() << " dist : " << _exactDistance << " within ? " << _exactWithin;
        }

        BSONObj _key;
        DiskLoc _loc;
        BSONObj _o;

        double _exactDistance;
        bool _exactWithin;

    };

    class GeoAccumulator {
    public:
        GeoAccumulator( const Geo2dType * g , const BSONObj& filter )
            : _g(g) , _lookedAt(0) , _objectsLoaded(0) , _found(0) {
            if ( ! filter.isEmpty() ) {
                _matcher.reset( new CoveredIndexMatcher( filter , g->keyPattern() ) );
            }
        }

        virtual ~GeoAccumulator() {
        }

        virtual void add( const KeyNode& node ) {

            GEODEBUG( "\t\t\t\t checking key " << node.key.toString() )

            // when looking at other boxes, don't want to look at some key/object pair twice
            pair<set<pair<const char*, DiskLoc> >::iterator,bool> seenBefore = _seen.insert( make_pair(node.key.objdata(), node.recordLoc) );
            if ( ! seenBefore.second ) {
                GEODEBUG( "\t\t\t\t already seen : " << node.key.toString() << " @ " << Point( _g, GeoHash( node.key.firstElement() ) ).toString() << " with " << node.recordLoc.obj()["_id"] );
                return;
            }
            _lookedAt++;

            // distance check
            double d = 0;
            if ( ! checkDistance( node , d ) ) {
                GEODEBUG( "\t\t\t\t bad distance : " << node.recordLoc.obj()  << "\t" << d );
                return;
            }
            GEODEBUG( "\t\t\t\t good distance : " << node.recordLoc.obj()  << "\t" << d );

            // Remember match results for each object
            map<DiskLoc, bool>::iterator match = _matched.find( node.recordLoc );
            bool newDoc = match == _matched.end();
            if( newDoc ) {

                // matcher
                MatchDetails details;
                if ( _matcher.get() ) {
                    bool good = _matcher->matchesWithSingleKeyIndex( node.key , node.recordLoc , &details );
                    if ( details.loadedObject )
                        _objectsLoaded++;

                    if ( ! good ) {
                        GEODEBUG( "\t\t\t\t didn't match : " << node.recordLoc.obj()["_id"] );
                        _matched[ node.recordLoc ] = false;
                        return;
                    }
                }

                _matched[ node.recordLoc ] = true;

                if ( ! details.loadedObject ) // don't double count
                    _objectsLoaded++;

            }
            else if( !((*match).second) ) {
                GEODEBUG( "\t\t\t\t previously didn't match : " << node.recordLoc.obj()["_id"] );
                return;
            }

            addSpecific( node , d, newDoc );
            _found++;
        }

        virtual void addSpecific( const KeyNode& node , double d, bool newDoc ) = 0;
        virtual bool checkDistance( const KeyNode& node , double& d ) = 0;

        long long found() const {
            return _found;
        }

        const Geo2dType * _g;
        set< pair<const char*, DiskLoc> > _seen;
        map<DiskLoc, bool> _matched;
        auto_ptr<CoveredIndexMatcher> _matcher;

        long long _lookedAt;
        long long _objectsLoaded;
        long long _found;
    };

    class GeoHopper : public GeoAccumulator {
    public:
        typedef multiset<GeoPoint> Holder;

        GeoHopper( const Geo2dType * g , unsigned max , const Point& n , const BSONObj& filter = BSONObj() , double maxDistance = numeric_limits<double>::max() , GeoDistType type=GEO_PLAIN )
            : GeoAccumulator( g , filter ) , _max( max ) , _near( n ), _maxDistance( maxDistance ), _type( type ), _distError( type == GEO_PLAIN ? g->_error : g->_errorSphere ), _farthest(0)
        {}

        virtual bool checkDistance( const KeyNode& node, double& d ) {

            // Always check approximate distance, since it lets us avoid doing
            // checks of the rest of the object if it succeeds
            // TODO:  Refactor so that we can check exact distance and within if we are going to
            // anyway.
            d = approxDistance( node );
            assert( d >= 0 );

            // Out of the error range, see how close we are to the furthest points
            bool good = d <= _maxDistance + 2 * _distError /* In error range */
            		     && ( _points.size() < _max /* need more points */
            		       || d <= farthest() + 2 * _distError /* could be closer than previous points */ );

            GEODEBUG( "\t\t\t\t\t\t\t checkDistance " << _near.toString()
                      << "\t" << GeoHash( node.key.firstElement() ) << "\t" << d
                      << " ok: " << good << " farthest: " << farthest() );

            return good;
        }

        double approxDistance( const KeyNode& node ) {
            return approxDistance( GeoHash( node.key.firstElement() ) );
        }

        double approxDistance( const GeoHash& h ) {

            double approxDistance = -1;
            switch (_type) {
            case GEO_PLAIN:
                approxDistance = _near.distance( Point( _g, h ) );
                break;
            case GEO_SPHERE:
                approxDistance = spheredist_deg( _near, Point( _g, h ) );
                break;
            default: assert( false );
            }

            return approxDistance;
        }

        double exactDistances( const KeyNode& node ) {

            GEODEBUG( "Finding exact distance for " << node.key.toString() << " and " << node.recordLoc.obj().toString() );

            // Find all the location objects from the keys
            vector< BSONObj > locs;
            _g->getKeys( node.recordLoc.obj(), locs );

            //double maxDistance = -1;

            // Find the particular location we want
            BSONObj loc;
            GeoHash keyHash( node.key.firstElement(), _g->_bits );
	    bool eWithin = false;
            double minDistance = -1;
	    for( vector< BSONObj >::iterator i = locs.begin(); i != locs.end(); ++i ) {

                loc = *i;

                // Ignore all locations not hashed to the key's hash, since we may see
                // those later
                //if( _g->_hash( loc ) != keyHash ) continue;

                double exactDistance = -1;
                bool exactWithin = false;

                // Get the appropriate distance for the type
                switch ( _type ) {
                case GEO_PLAIN:
                    exactDistance = _near.distance( Point( loc ) );
                    exactWithin = _near.distanceWithin( Point( loc ), _maxDistance );
                    break;
                case GEO_SPHERE:
                    exactDistance = spheredist_deg( _near, Point( loc ) );
                    exactWithin = ( exactDistance <= _maxDistance );
                    break;
                default: assert( false );
                }

                assert( exactDistance >= 0 );
                if( !exactWithin ) continue;

                GEODEBUG( "Inserting exact point: " << GeoPoint( node , exactDistance, exactWithin ) );
		if( minDistance < 0 || minDistance > exactDistance ) {
			minDistance = exactDistance;
			eWithin = exactWithin;
		}
                //if( exactDistance > maxDistance ) maxDistance = exactDistance;
            }
                // Add a point for this location
	    if( minDistance >= 0 )
		    _points.insert( GeoPoint( node , minDistance, eWithin ) );

            return minDistance;
            //return maxDistance;

        }

        // Always in distance units, whether radians or normal
        double farthest() const {
            return _farthest;
        }

        bool inErrorBounds( double approxD ) const {
            return approxD >= _maxDistance - _distError && approxD <= _maxDistance + _distError;
        }

        virtual void addSpecific( const KeyNode& node , double d, bool newDoc ) {
            if( ! newDoc ) return;

            GEODEBUG( "\t\t" << GeoHash( node.key.firstElement() ) << "\t" << node.recordLoc.obj() << "\t" << d );

            double maxDistance = exactDistances( node );
            if( maxDistance >= 0 ){

            	// Recalculate the current furthest point.
            	int numToErase = _points.size() - _max;
				while( numToErase-- > 0 ){
					_points.erase( --_points.end() );
				}

				_farthest = boost::next( _points.end(), -1 )->_exactDistance;

            }
        }

        unsigned _max;
        Point _near;
        Holder _points;
        double _maxDistance;
        GeoDistType _type;
        double _distError;
        double _farthest;

    };


    struct BtreeLocation {
        int pos;
        bool found;
        DiskLoc bucket;

        BSONObj key() {
            if ( bucket.isNull() )
                return BSONObj();
            return bucket.btree()->keyNode( pos ).key;
        }

        bool hasPrefix( const GeoHash& hash ) {
            BSONElement e = key().firstElement();
            if ( e.eoo() )
                return false;
            return GeoHash( e ).hasPrefix( hash );
        }

        bool advance( int direction , int& totalFound , GeoAccumulator* all ) {

            if ( bucket.isNull() )
                return false;
            bucket = bucket.btree()->advance( bucket , pos , direction , "btreelocation" );

            if ( all )
                return checkCur( totalFound , all );

            return ! bucket.isNull();
        }

        bool checkCur( int& totalFound , GeoAccumulator* all ) {
            if ( bucket.isNull() )
                return false;

            if ( bucket.btree()->isUsed(pos) ) {
                totalFound++;
                all->add( bucket.btree()->keyNode( pos ) );
            }
            else {
                GEODEBUG( "\t\t\t\t not used: " << key() );
            }

            return true;
        }

        string toString() {
            stringstream ss;
            ss << "bucket: " << bucket.toString() << " pos: " << pos << " found: " << found;
            return ss.str();
        }

        // Returns the min and max keys which bound a particular location.
        // The only time these may be equal is when we actually equal the location
        // itself, otherwise our expanding algorithm will fail.
        static bool initial( const IndexDetails& id , const Geo2dType * spec ,
                             BtreeLocation& min , BtreeLocation&  max ,
                             GeoHash start ,
                             int & found , GeoAccumulator * hopper ) {

            Ordering ordering = Ordering::make(spec->_order);

            min.bucket = id.head.btree()->locate( id , id.head , start.wrap() ,
                                                  ordering , min.pos , min.found , minDiskLoc, -1 );

            if (hopper) min.checkCur( found , hopper );

            // TODO: Might be able to avoid doing a full lookup in some cases here,
            // but would add complexity and we're hitting pretty much the exact same data.
            // Cannot set this = min in general, however.
            max.bucket = id.head.btree()->locate( id , id.head , start.wrap() ,
                                                  ordering , max.pos , max.found , minDiskLoc, 1 );

            if (hopper) max.checkCur( found , hopper );

            return ! min.bucket.isNull() || ! max.bucket.isNull();
        }
    };

    class GeoSearch {
    public:
        GeoSearch( const Geo2dType * g , const Point& startPt , int numWanted=100 , BSONObj filter=BSONObj() , double maxDistance = numeric_limits<double>::max() , GeoDistType type=GEO_PLAIN )
            : _spec( g ) ,_startPt( startPt ), _start( g->hash( startPt._x, startPt._y ) ) ,
              _numWanted( numWanted ) , _filter( filter ) , _maxDistance( maxDistance ) ,
              _hopper( new GeoHopper( g , numWanted , _startPt , filter , maxDistance, type ) ), _type(type) {
            assert( g->getDetails() );
            _nscanned = 0;
            _found = 0;

            if (type == GEO_PLAIN) {
                _scanDistance = _maxDistance + _spec->_error;
            }
            else if (type == GEO_SPHERE) {
                if (_maxDistance == numeric_limits<double>::max()) {
                    _scanDistance = _maxDistance;
                }
                else {
                    //TODO: consider splitting into x and y scan distances
                    _scanDistance = computeXScanDistance(_startPt._y, rad2deg( _maxDistance ) + _spec->_error );
                }
            }
            else {
                assert(0);
            }
        }

        void exec() {
            const IndexDetails& id = *_spec->getDetails();

            const BtreeBucket * head = id.head.btree();
            assert( head );
            /*
             * Search algorithm
             * 1) use geohash prefix to find X items
             * 2) compute max distance from want to an item
             * 3) find optimal set of boxes that complete circle
             * 4) use regular btree cursors to scan those boxes
             */

            GeoHopper * hopper = _hopper.get();

	    _prefix = _start;

            BtreeLocation min,max;
            {
                // TODO : Refactor this into the other, tested, geohash algorithm

                // 1 regular geo hash algorithm
                if ( ! BtreeLocation::initial( id , _spec , min , max , _start , _found , NULL ) )
                    return;

                while ( !_prefix.constrains() || // if next pass would cover universe, just keep going
                        ( _hopper->found() < _numWanted && _spec->sizeEdge( _prefix ) <= _scanDistance)) {
                    GEODEBUG( _prefix << "\t" << _found << "\t DESC" );
		    while ( min.hasPrefix(_prefix) && min.checkCur(_found, hopper) && min.advance(-1, _found, NULL) )
                        _nscanned++;
                    GEODEBUG( _prefix << "\t" << _found << "\t ASC" );
                    while ( max.hasPrefix(_prefix) && max.checkCur(_found, hopper) && max.advance(+1, _found, NULL) )
                        _nscanned++;

                    if ( ! _prefix.constrains() ) {
                        GEODEBUG( "done search w/o part 2" )
                        return;
                    }

                    _alreadyScanned = Box(_spec, _prefix);
                    _prefix = _prefix.up();
                }
            }
            GEODEBUG( "done part 1" );
            {
                // 2
                double farthest = hopper->farthest();
                GEODEBUGPRINT(hopper->farthest());
                if (hopper->found() < _numWanted) {
                    // Not enough found in Phase 1
                    farthest = _scanDistance;
                }
                else if (_type == GEO_PLAIN) {
                    farthest += _spec->_error;
                }
                else if (_type == GEO_SPHERE) {
                    farthest = std::min( _scanDistance, computeXScanDistance(_startPt._y, rad2deg(farthest)) + 2 * _spec->_error );
                }
                assert( farthest >= 0 );
                GEODEBUGPRINT(farthest);

                Box want( _startPt._x - farthest , _startPt._y - farthest , farthest * 2 );
                GEODEBUGPRINT(want.toString());

                _prefix = _start;
                while (_prefix.constrains() && _spec->sizeEdge( _prefix ) < farthest ) {
                    _prefix = _prefix.up();
                }

                PREFIXDEBUG(_prefix, _spec);

                if (_prefix.getBits() <= 1) {
                    // TODO consider walking in $natural order

                    while ( min.checkCur(_found, hopper) && min.advance(-1, _found, NULL) )
                        _nscanned++;
                    while ( max.checkCur(_found, hopper) && max.advance(+1, _found, NULL) )
                        _nscanned++;

                    GEODEBUG( "done search after scanning whole collection" )
                    return;
                }

                if ( logLevel > 0 ) {
                    log(1) << "want: " << want << " found:" << _found << " nscanned: " << _nscanned << " hash size:" << _spec->sizeEdge( _prefix )
                           << " farthest: " << farthest << " using box: " << Box( _spec , _prefix ).toString() << endl;
                }

                for ( int x=-1; x<=1; x++ ) {
                    for ( int y=-1; y<=1; y++ ) {
                        GeoHash toscan = _prefix;
                        toscan.move( x , y );

                        // 3 & 4
                        doBox( id , want , toscan );
                    }
                }
            }
            GEODEBUG( "done search" )

        }

        void doBox( const IndexDetails& id , const Box& want , const GeoHash& toscan , int depth = 0 ) {
            Box testBox( _spec , toscan );
            if ( logLevel > 2 ) {
                cout << "\t";
                for ( int i=0; i<depth; i++ )
                    cout << "\t";
                cout << " doBox: " << testBox.toString() << "\t" << toscan.toString() << " scanned so far: " << _nscanned << endl;
            }
            else {
                GEODEBUGPRINT(testBox.toString());
            }

            if ( _alreadyScanned.area() > 0 && _alreadyScanned.contains( testBox ) ) {
                GEODEBUG( "skipping box : already scanned box " << _alreadyScanned.toString() );
                return; // been here, done this
            }

            double intPer = testBox.intersects( want );

            if ( intPer <= 0 ) {
                GEODEBUG("skipping box: not in want");
                return;
            }

            bool goDeeper = intPer < .5 && depth < 2;

            long long myscanned = 0;

            BtreeLocation loc;
            loc.bucket = id.head.btree()->locate( id , id.head , toscan.wrap() , Ordering::make(_spec->_order) ,
                                                  loc.pos , loc.found , minDiskLoc );
            loc.checkCur( _found , _hopper.get() );
            while ( loc.hasPrefix( toscan ) && loc.advance( 1 , _found , _hopper.get() ) ) {
                _nscanned++;
                if ( ++myscanned > 100 && goDeeper ) {
                    doBox( id , want , toscan + "00" , depth + 1);
                    doBox( id , want , toscan + "01" , depth + 1);
                    doBox( id , want , toscan + "10" , depth + 1);
                    doBox( id , want , toscan + "11" , depth + 1);
                    return;
                }
            }

        }


        const Geo2dType * _spec;

        Point _startPt;
        GeoHash _start;
        GeoHash _prefix;
        int _numWanted;
        BSONObj _filter;
        double _maxDistance;
        double _scanDistance;
        shared_ptr<GeoHopper> _hopper;

        long long _nscanned;
        int _found;
        GeoDistType _type;

        Box _alreadyScanned;
    };

    class GeoCursorBase : public Cursor {
    public:
        GeoCursorBase( const Geo2dType * spec )
            : _spec( spec ), _id( _spec->getDetails() ) {

        }

        virtual DiskLoc refLoc() { return DiskLoc(); }

        virtual BSONObj indexKeyPattern() {
            return _spec->keyPattern();
        }

        virtual void noteLocation() {
            // no-op since these are meant to be safe
        }

        /* called before query getmore block is iterated */
        virtual void checkLocation() {
            // no-op since these are meant to be safe
        }

        virtual bool supportGetMore() { return false; }
        virtual bool supportYields() { return false; }

        virtual bool getsetdup(DiskLoc loc) { return false; }
        virtual bool modifiedKeys() const { return true; }
        virtual bool isMultiKey() const { return false; }



        const Geo2dType * _spec;
        const IndexDetails * _id;
    };

    class GeoSearchCursor : public GeoCursorBase {
    public:
        GeoSearchCursor( shared_ptr<GeoSearch> s )
            : GeoCursorBase( s->_spec ) ,
              _s( s ) , _cur( s->_hopper->_points.begin() ) , _end( s->_hopper->_points.end() ), _nscanned() {
            if ( _cur != _end ) {
                ++_nscanned;
            }
        }

        virtual ~GeoSearchCursor() {}

        virtual bool ok() {
            return _cur != _end;
        }

        virtual Record* _current() { assert(ok()); return _cur->_loc.rec(); }
        virtual BSONObj current() { assert(ok()); return _cur->_o; }
        virtual DiskLoc currLoc() { assert(ok()); return _cur->_loc; }
        virtual bool advance() { _cur++; incNscanned(); return ok(); }
        virtual BSONObj currKey() const { return _cur->_key; }

        virtual string toString() {
            return "GeoSearchCursor";
        }


        virtual BSONObj prettyStartKey() const {
            return BSON( _s->_spec->_geo << _s->_prefix.toString() );
        }
        virtual BSONObj prettyEndKey() const {
            GeoHash temp = _s->_prefix;
            temp.move( 1 , 1 );
            return BSON( _s->_spec->_geo << temp.toString() );
        }

        virtual long long nscanned() { return _nscanned; }

        virtual CoveredIndexMatcher *matcher() const {
            return _s->_hopper->_matcher.get();
        }

        shared_ptr<GeoSearch> _s;
        GeoHopper::Holder::iterator _cur;
        GeoHopper::Holder::iterator _end;

        void incNscanned() { if ( ok() ) { ++_nscanned; } }
        long long _nscanned;
    };

    class GeoBrowse : public GeoCursorBase , public GeoAccumulator {
    public:

        // The max points which should be added to an expanding box
        static const int maxPointsHeuristic = 300;

        // Expand states
        enum State {
            START ,
            DOING_EXPAND ,
            DONE_NEIGHBOR ,
            DONE
        } _state;

        GeoBrowse( const Geo2dType * g , string type , BSONObj filter = BSONObj() )
            : GeoCursorBase( g ) ,GeoAccumulator( g , filter ) ,
              _type( type ) , _filter( filter ) , _firstCall(true), _nscanned(), _centerPrefix(0, 0, 0) {

            // Set up the initial expand state
            _state = START;
            _neighbor = -1;
            _found = 0;

        }

        virtual string toString() {
            return (string)"GeoBrowse-" + _type;
        }

        virtual bool ok() {
            bool first = _firstCall;
            if ( _firstCall ) {
                fillStack( maxPointsHeuristic );
                _firstCall = false;
            }
            if ( ! _cur.isEmpty() || _stack.size() ) {
                if ( first ) {
                    ++_nscanned;
                }
                return true;
            }

            while ( moreToDo() ) {
                fillStack( maxPointsHeuristic );
                if ( ! _cur.isEmpty() ) {
                    if ( first ) {
                        ++_nscanned;
                    }
                    return true;
                }
            }

            return false;
        }

        virtual bool advance() {
            _cur._o = BSONObj();

            if ( _stack.size() ) {
                _cur = _stack.front();
                _stack.pop_front();
                ++_nscanned;
                return true;
            }

            if ( ! moreToDo() )
                return false;

            while ( _cur.isEmpty() && moreToDo() )
                fillStack( maxPointsHeuristic );
            return ! _cur.isEmpty() && ++_nscanned;
        }

        virtual Record* _current() { assert(ok()); return _cur._loc.rec(); }
        virtual BSONObj current() { assert(ok()); return _cur._o; }
        virtual DiskLoc currLoc() { assert(ok()); return _cur._loc; }
        virtual BSONObj currKey() const { return _cur._key; }


        virtual CoveredIndexMatcher *matcher() const {
            return _matcher.get();
        }

        // Are we finished getting points?
        virtual bool moreToDo() {
            return _state != DONE;
        }

        // Fills the stack, but only checks a maximum number of maxToCheck points at a time.
        // Further calls to this function will continue the expand/check neighbors algorithm.
        virtual void fillStack( int maxToCheck ) {

#ifdef GEODEBUGGING

            int s = _state;
            log() << "Filling stack with maximum of " << maxToCheck << ", state : " << s << endl;

#endif

            int maxFound = _found + maxToCheck;
            bool isNeighbor = _centerPrefix.constrains();

            // Starting a box expansion
            if ( _state == START ) {

                // Get the very first hash point, if required
                if( ! isNeighbor )
                    _prefix = expandStartHash();

                GEODEBUG( "initializing btree" );

                if ( ! BtreeLocation::initial( *_id , _spec , _min , _max , _prefix , _found , this ) )
                    _state = isNeighbor ? DONE_NEIGHBOR : DONE;
                else
                    _state = DOING_EXPAND;

                GEODEBUG( (_state == DONE_NEIGHBOR || _state == DONE ? "not initialized" : "initializedFig") );

            }

            // Doing the actual box expansion
            if ( _state == DOING_EXPAND ) {

                while ( true ) {

                    GEODEBUG( "box prefix [" << _prefix << "]" );

#ifdef GEODEBUGGING
                    if( _prefix.constrains() ) {
                        Box in( _g , GeoHash( _prefix ) );
                        log() << "current expand box : " << in.toString() << endl;
                    }
                    else {
                        log() << "max expand box." << endl;
                    }
#endif

                    GEODEBUG( "expanding box points... ");

                    // Find points inside this prefix
                    while ( _min.hasPrefix( _prefix ) && _min.advance( -1 , _found , this ) && _found < maxFound );
                    while ( _max.hasPrefix( _prefix ) && _max.advance( 1 , _found , this ) && _found < maxFound );

                    GEODEBUG( "finished expand, found : " << ( maxToCheck - ( maxFound - _found ) ) );

                    if( _found >= maxFound ) return;

                    // If we've searched the entire space, we're finished.
                    if ( ! _prefix.constrains() ) {
                        GEODEBUG( "box exhausted" );
                        _state = DONE;
                        return;
                    }

                    if ( ! fitsInBox( _g->sizeEdge( _prefix ) ) ) {

                        // If we're still not expanded bigger than the box size, expand again
                        // TODO: Is there an advantage to scanning prior to expanding?
                        _prefix = _prefix.up();
                        continue;

                    }

                    // We're done and our size is large enough
                    _state = DONE_NEIGHBOR;
                    // Go to the next neighbor
                    _neighbor++;
                    break;

                }
            }

            // If we're done expanding the current box...
            if( _state == DONE_NEIGHBOR ) {

                // Iterate to the next neighbor
                // Loop is useful for cases where we want to skip over boxes entirely,
                // otherwise recursion increments the neighbors.
                for ( ; _neighbor < 9; _neighbor++ ) {

                    if( ! isNeighbor ) {
                        _centerPrefix = _prefix;
                        _centerBox = Box( _g, _centerPrefix );
                        isNeighbor = true;
                    }

                    int i = (_neighbor / 3) - 1;
                    int j = (_neighbor % 3) - 1;

                    if ( ( i == 0 && j == 0 ) ||
                            ( i < 0 && _centerBox._min._x <= _g->_min ) ||
                            ( j < 0 && _centerBox._min._y <= _g->_min ) ||
                            ( i > 0 && _centerBox._max._x >= _g->_max ) ||
                            ( j > 0 && _centerBox._max._y >= _g->_max ) ) {
                        continue; // main box or wrapped edge
                        // TODO:  We may want to enable wrapping in future, probably best as layer on top of
                        // this search.
                    }

                    // Make sure we've got a reasonable center
                    assert( _centerPrefix.constrains() );

                    GeoHash newBox = _centerPrefix;
                    newBox.move( i, j );
                    _prefix = newBox;

                    GEODEBUG( "moving to " << i << " , " << j );
                    PREFIXDEBUG( _centerPrefix, _g );
                    PREFIXDEBUG( newBox , _g );

                    Box cur( _g , newBox );
                    if ( intersectsBox( cur ) ) {

                        // TODO: Consider exploiting recursion to do an expand search
                        // from a smaller region

                        // Restart our search from a diff box.
                        _state = START;

                        fillStack( maxFound - _found );

                        // When we return from the recursive fillStack call, we'll either have checked enough points or
                        // be entirely done.  Max recurse depth is 8.

                        // If we're maxed out on points, return
                        if( _found >= maxFound ) {
                            // Make sure we'll come back to add more points
                            assert( _state == DOING_EXPAND );
                            return;
                        }

                        // Otherwise we must be finished to return
                        assert( _state == DONE );
                        return;

                    }
                    else {
                        GEODEBUG( "skipping box" );
                        continue;
                    }

                }

                // Finished with neighbors
                _state = DONE;
            }

        }

        // The initial geo hash box for our first expansion
        virtual GeoHash expandStartHash() = 0;

        // Whether the current box width is big enough for our search area
        virtual bool fitsInBox( double width ) = 0;

        // Whether the current box overlaps our search area
        virtual bool intersectsBox( Box& cur ) = 0;

        virtual void addSpecific( const KeyNode& node , double d, bool newDoc ) {

            if( ! newDoc ) return;

            if ( _cur.isEmpty() )
                _cur = GeoPoint( node );
            else
                _stack.push_back( GeoPoint( node ) );
        }

        virtual long long nscanned() {
            if ( _firstCall ) {
                ok();
            }
            return _nscanned;
        }

        string _type;
        BSONObj _filter;
        list<GeoPoint> _stack;

        GeoPoint _cur;
        bool _firstCall;

        long long _nscanned;

        // The current box we're expanding (-1 is first/center box)
        int _neighbor;

        // The points we've found so far
        int _found;

        // The current hash prefix we're expanding and the center-box hash prefix
        GeoHash _prefix;
        GeoHash _centerPrefix;
        Box _centerBox;

        // Start and end of our search range in the current box
        BtreeLocation _min;
        BtreeLocation _max;

    };

    class GeoCircleBrowse : public GeoBrowse {
    public:

        GeoCircleBrowse( const Geo2dType * g , const BSONObj& circle , BSONObj filter = BSONObj() , const string& type="$center")
            : GeoBrowse( g , "circle" , filter ) {

            uassert( 13060 , "$center needs 2 fields (middle,max distance)" , circle.nFields() == 2 );

            BSONObjIterator i(circle);
            BSONElement center = i.next();

            uassert( 13656 , "the first field of $center object must be a location object" , center.isABSONObj() );

            // Get geohash and exact center point
            // TODO: For wrapping search, may be useful to allow center points outside-of-bounds here.
            // Calculating the nearest point as a hash start inside the region would then be required.
            _start = g->_tohash(center);
            _startPt = Point(center);

            _maxDistance = i.next().numberDouble();
            uassert( 13061 , "need a max distance > 0 " , _maxDistance > 0 );

            if (type == "$center") {
                // Look in box with bounds of maxDistance in either direction
                _type = GEO_PLAIN;
                _xScanDistance = _maxDistance + _g->_error;
                _yScanDistance = _maxDistance + _g->_error;
            }
            else if (type == "$centerSphere") {
                // Same, but compute maxDistance using spherical transform

                uassert(13461, "Spherical MaxDistance > PI. Are you sure you are using radians?", _maxDistance < M_PI);

                _type = GEO_SPHERE;
                _yScanDistance = rad2deg( _maxDistance ) + _g->_error;
                _xScanDistance = computeXScanDistance(_startPt._y, _yScanDistance);

                uassert(13462, "Spherical distance would require wrapping, which isn't implemented yet",
                        (_startPt._x + _xScanDistance < 180) && (_startPt._x - _xScanDistance > -180) &&
                        (_startPt._y + _yScanDistance < 90) && (_startPt._y - _yScanDistance > -90));
            }
            else {
                uassert(13460, "invalid $center query type: " + type, false);
            }

            // Bounding box includes fudge factor.
            // TODO:  Is this correct, since fudge factor may be spherically transformed?
            _bBox._min = Point( _startPt._x - _xScanDistance, _startPt._y - _yScanDistance );
            _bBox._max = Point( _startPt._x + _xScanDistance, _startPt._y + _yScanDistance );

            GEODEBUG( "Bounding box for circle query : " << _bBox.toString() << " (max distance : " << _maxDistance << ")" << " starting from " << _startPt.toString() );

            ok();
        }

        virtual GeoHash expandStartHash() {
            return _start;
        }

        virtual bool fitsInBox( double width ) {
            return width >= std::max(_xScanDistance, _yScanDistance);
        }

        virtual bool intersectsBox( Box& cur ) {
            return _bBox.intersects( cur );
        }

        virtual bool checkDistance( const KeyNode& node, double& d ) {

            GeoHash h( node.key.firstElement(), _g->_bits );

            // Inexact hash distance checks.
            double error = 0;
            switch (_type) {
            case GEO_PLAIN:
                d = _g->distance( _start , h );
                error = _g->_error;
                break;
            case GEO_SPHERE:
                d = spheredist_deg( _startPt, Point( _g, h ) );
                error = _g->_errorSphere;
                break;
            default: assert( false );
            }

            // If our distance is in the error bounds...
            if( d >= _maxDistance - error && d <= _maxDistance + error ) {

                // Do exact check
                vector< BSONObj > locs;
                _g->getKeys( node.recordLoc.obj(), locs );

                for( vector< BSONObj >::iterator i = locs.begin(); i != locs.end(); ++i ) {

                    GEODEBUG( "Inexact distance : " << d << " vs " << _maxDistance << " from " << ( *i ).toString() << " due to error " << error );

                    // Exact distance checks.
                    switch (_type) {
                    case GEO_PLAIN: {
                        if( _startPt.distanceWithin( Point( *i ), _maxDistance ) ) return true;
                        break;
                    }
                    case GEO_SPHERE:
                        // Ignore all locations not hashed to the key's hash, since spherical calcs are
                        // more expensive.
                        if( _g->_hash( *i ) != h ) break;
                        if( spheredist_deg( _startPt , Point( *i ) ) <= _maxDistance ) return true;
                        break;
                    default: assert( false );
                    }

                }

                return false;
            }

            GEODEBUG( "\t " << h << "\t" << d );
            return d <= _maxDistance;
        }

        GeoDistType _type;
        GeoHash _start;
        Point _startPt;
        double _maxDistance; // user input
        double _xScanDistance; // effected by GeoDistType
        double _yScanDistance; // effected by GeoDistType
        Box _bBox;

    };

    class GeoBoxBrowse : public GeoBrowse {
    public:

        GeoBoxBrowse( const Geo2dType * g , const BSONObj& box , BSONObj filter = BSONObj() )
            : GeoBrowse( g , "box" , filter ) {

            uassert( 13063 , "$box needs 2 fields (bottomLeft,topRight)" , box.nFields() == 2 );

            // Initialize an *exact* box from the given obj.
            BSONObjIterator i(box);
            _want._min = Point( i.next() );
            _want._max = Point( i.next() );

            fixBox( g, _want );

            uassert( 13064 , "need an area > 0 " , _want.area() > 0 );

            Point center = _want.center();
            _start = _g->hash( center._x , center._y );

            GEODEBUG( "center : " << center.toString() << "\t" << _prefix );

            _fudge = _g->_error;
            _wantLen = _fudge +
                       std::max( ( _want._max._x - _want._min._x ) ,
                                 ( _want._max._y - _want._min._y ) );

            ok();
        }

        void fixBox( const Geo2dType* g, Box& box ) {
            if( _want._min._x > _want._max._x )
                swap( _want._min._x, _want._max._x );
            if( _want._min._y > _want._max._y )
                swap( _want._min._y, _want._max._y );

            double gMin = g->_min;
            double gMax = g->_max;

            if( _want._min._x < gMin ) _want._min._x = gMin;
            if( _want._min._y < gMin ) _want._min._y = gMin;
            if( _want._max._x > gMax) _want._max._x = gMax;
            if( _want._max._y > gMax ) _want._max._y = gMax;
        }

        void swap( double& a, double& b ) {
            double swap = a;
            a = b;
            b = swap;
        }

        virtual GeoHash expandStartHash() {
            return _start;
        }

        virtual bool fitsInBox( double width ) {
            return width >= _wantLen;
        }

        virtual bool intersectsBox( Box& cur ) {
            return _want.intersects( cur );
        }

        virtual bool checkDistance( const KeyNode& node, double& d ) {

            GeoHash h( node.key.firstElement() );
            Point approxPt( _g, h );

            bool approxInside = _want.inside( approxPt, _fudge );

            if( approxInside && _want.onBoundary( approxPt, _fudge ) ) {

                // Do exact check
                vector< BSONObj > locs;
                _g->getKeys( node.recordLoc.obj(), locs );

                for( vector< BSONObj >::iterator i = locs.begin(); i != locs.end(); ++i ) {
                    if( _want.inside( Point( *i ) ) ) {

                        GEODEBUG( "found exact point : " << _want.toString()
                                  << " exact point : " << Point( *i ).toString()
                                  << " approx point : " << approxPt.toString()
                                  << " because of error: " << _fudge );

                        return true;
                    }
                }

                return false;
            }

            GEODEBUG( "checking point : " << _want.toString()
                      << " point: " << approxPt.toString()
                      << " in : " << _want.inside( approxPt, _fudge ) );

            return approxInside;
        }

	Box _want;
	double _wantLen;
        double _fudge;

        GeoHash _start;

    };


    class GeoPolygonBrowse : public GeoBrowse {
    public:

        GeoPolygonBrowse( const Geo2dType* g , const BSONObj& polyPoints ,
                          BSONObj filter = BSONObj() ) : GeoBrowse( g , "polygon" , filter ) {

            GEODEBUG( "In Polygon" )

            BSONObjIterator i( polyPoints );
            BSONElement first = i.next();
            _poly.add( Point( first ) );

            while ( i.more() ) {
                _poly.add( Point( i.next() ) );
            }

            uassert( 14030, "polygon must be defined by three points or more", _poly.size() >= 3 );

            _bounds = _poly.bounds();
            _maxDim = _bounds.maxDim();

            ok();
        }

        // The initial geo hash box for our first expansion
        virtual GeoHash expandStartHash() {
            return _g->hash( _poly.centroid() );
        }

        // Whether the current box width is big enough for our search area
        virtual bool fitsInBox( double width ) {
            return _maxDim <= width;
        }

        // Whether the current box overlaps our search area
        virtual bool intersectsBox( Box& cur ) {
            return _bounds.intersects( cur );
        }

        virtual bool checkDistance( const KeyNode& node, double& d ) {

            GeoHash h( node.key.firstElement(), _g->_bits );
            Point p( _g, h );

            int in = _poly.contains( p, _g->_error );
            if( in != 0 ) {

                if ( in > 0 ) {
                    GEODEBUG( "Point: [" << p._x << ", " << p._y << "] approx in polygon" );
                }
                else {
                    GEODEBUG( "Point: [" << p._x << ", " << p._y << "] approx not in polygon" );
                }

                if( in != 0 ) return in > 0;
            }

            // Do exact check, since to approximate check was inconclusive
            vector< BSONObj > locs;
            _g->getKeys( node.recordLoc.obj(), locs );

            for( vector< BSONObj >::iterator i = locs.begin(); i != locs.end(); ++i ) {

                Point p( *i );

                // Ignore all points not hashed to the current value
                // This implicitly assumes hashing is less costly than the polygon check, which
                // may or may not be true.
                if( _g->hash( p ) != h ) continue;

                // Use the point in polygon algorithm to see if the point
                // is contained in the polygon.
                bool in = _poly.contains( p );
                if ( in ) {
                    GEODEBUG( "Point: [" << p._x << ", " << p._y << "] exactly in polygon" );
                }
                else {
                    GEODEBUG( "Point: [" << p._x << ", " << p._y << "] exactly not in polygon" );
                }
                if( in ) return in;

            }

            return false;
        }

    private:

        Polygon _poly;
        Box _bounds;
        double _maxDim;

        GeoHash _start;

    };


    shared_ptr<Cursor> Geo2dType::newCursor( const BSONObj& query , const BSONObj& order , int numWanted ) const {
        if ( numWanted < 0 )
            numWanted = numWanted * -1;
        else if ( numWanted == 0 )
            numWanted = 100;

        BSONObjIterator i(query);
        while ( i.more() ) {
            BSONElement e = i.next();

            if ( _geo != e.fieldName() )
                continue;

            if ( e.type() != Object )
                continue;

            switch ( e.embeddedObject().firstElement().getGtLtOp() ) {
            case BSONObj::opNEAR: {
                BSONObj n = e.embeddedObject();
                e = n.firstElement();

                const char* suffix = e.fieldName() + 5; // strlen("$near") == 5;
                GeoDistType type;
                if (suffix[0] == '\0') {
                    type = GEO_PLAIN;
                }
                else if (strcmp(suffix, "Sphere") == 0) {
                    type = GEO_SPHERE;
                }
                else {
                    uassert(13464, string("invalid $near search type: ") + e.fieldName(), false);
                    type = GEO_PLAIN; // prevents uninitialized warning
                }

                double maxDistance = numeric_limits<double>::max();
                if ( e.isABSONObj() && e.embeddedObject().nFields() > 2 ) {
                    BSONObjIterator i(e.embeddedObject());
                    i.next();
                    i.next();
                    BSONElement e = i.next();
                    if ( e.isNumber() )
                        maxDistance = e.numberDouble();
                }
                {
                    BSONElement e = n["$maxDistance"];
                    if ( e.isNumber() )
                        maxDistance = e.numberDouble();
                }
                shared_ptr<GeoSearch> s( new GeoSearch( this , Point( e ) , numWanted , query , maxDistance, type ) );
                s->exec();
                shared_ptr<Cursor> c;
                c.reset( new GeoSearchCursor( s ) );
                return c;
            }
            case BSONObj::opWITHIN: {
                e = e.embeddedObject().firstElement();
                uassert( 13057 , "$within has to take an object or array" , e.isABSONObj() );
                e = e.embeddedObject().firstElement();
                string type = e.fieldName();
                if ( startsWith(type,  "$center") ) {
                    uassert( 13059 , "$center has to take an object or array" , e.isABSONObj() );
                    shared_ptr<Cursor> c( new GeoCircleBrowse( this , e.embeddedObjectUserCheck() , query , type) );
                    return c;
                }
                else if ( type == "$box" ) {
                    uassert( 13065 , "$box has to take an object or array" , e.isABSONObj() );
                    shared_ptr<Cursor> c( new GeoBoxBrowse( this , e.embeddedObjectUserCheck() , query ) );
                    return c;
                }
                else if ( startsWith( type, "$poly" ) ) {
                    uassert( 14029 , "$polygon has to take an object or array" , e.isABSONObj() );
                    shared_ptr<Cursor> c( new GeoPolygonBrowse( this , e.embeddedObjectUserCheck() , query ) );
                    return c;
                }
                throw UserException( 13058 , (string)"unknown $with type: " + type );
            }
            default:
                break;
            }
        }

        throw UserException( 13042 , (string)"missing geo field (" + _geo + ") in : " + query.toString() );
    }

    // ------
    // commands
    // ------

    class Geo2dFindNearCmd : public Command {
    public:
        Geo2dFindNearCmd() : Command( "geoNear" ) {}
        virtual LockType locktype() const { return READ; }
        bool slaveOk() const { return true; }
        void help(stringstream& h) const { h << "http://www.mongodb.org/display/DOCS/Geospatial+Indexing#GeospatialIndexing-geoNearCommand"; }
        bool slaveOverrideOk() { return true; }
        bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string ns = dbname + "." + cmdObj.firstElement().valuestr();

            NamespaceDetails * d = nsdetails( ns.c_str() );
            if ( ! d ) {
                errmsg = "can't find ns";
                return false;
            }

            vector<int> idxs;
            d->findIndexByType( GEO2DNAME , idxs );

            if ( idxs.size() > 1 ) {
                errmsg = "more than 1 geo indexes :(";
                return false;
            }

            if ( idxs.size() == 0 ) {
                errmsg = "no geo index :(";
                return false;
            }

            int geoIdx = idxs[0];

            result.append( "ns" , ns );

            IndexDetails& id = d->idx( geoIdx );
            Geo2dType * g = (Geo2dType*)id.getSpec().getType();
            assert( &id == g->getDetails() );

            int numWanted = 100;
            if ( cmdObj["num"].isNumber() )
                numWanted = cmdObj["num"].numberInt();

            uassert(13046, "'near' param missing/invalid", !cmdObj["near"].eoo());
            const Point n( cmdObj["near"] );
            result.append( "near" , g->_tohash( cmdObj["near"] ).toString() );

            BSONObj filter;
            if ( cmdObj["query"].type() == Object )
                filter = cmdObj["query"].embeddedObject();

            double maxDistance = numeric_limits<double>::max();
            if ( cmdObj["maxDistance"].isNumber() )
                maxDistance = cmdObj["maxDistance"].number();

            GeoDistType type = GEO_PLAIN;
            if ( cmdObj["spherical"].trueValue() )
                type = GEO_SPHERE;

            // We're returning exact distances, so don't evaluate lazily.
            GeoSearch gs( g , n , numWanted , filter , maxDistance , type );

            if ( cmdObj["start"].type() == String) {
                GeoHash start ((string) cmdObj["start"].valuestr());
                gs._start = start;
            }

            gs.exec();

            double distanceMultiplier = 1;
            if ( cmdObj["distanceMultiplier"].isNumber() )
                distanceMultiplier = cmdObj["distanceMultiplier"].number();

            double totalDistance = 0;


            BSONObjBuilder arr( result.subarrayStart( "results" ) );
            int x = 0;
            for ( GeoHopper::Holder::iterator i=gs._hopper->_points.begin(); i!=gs._hopper->_points.end(); i++ ) {

                const GeoPoint& p = *i;
                double dis = distanceMultiplier * p._exactDistance;
                totalDistance += dis;

                BSONObjBuilder bb( arr.subobjStart( BSONObjBuilder::numStr( x++ ) ) );
                bb.append( "dis" , dis );
                bb.append( "obj" , p._o );
                bb.done();
            }
            arr.done();

            BSONObjBuilder stats( result.subobjStart( "stats" ) );
            stats.append( "time" , cc().curop()->elapsedMillis() );
            stats.appendNumber( "btreelocs" , gs._nscanned );
            stats.appendNumber( "nscanned" , gs._hopper->_lookedAt );
            stats.appendNumber( "objectsLoaded" , gs._hopper->_objectsLoaded );
            stats.append( "avgDistance" , totalDistance / x );
            stats.append( "maxDistance" , gs._hopper->farthest() );
            stats.done();

            return true;
        }

    } geo2dFindNearCmd;

    class GeoWalkCmd : public Command {
    public:
        GeoWalkCmd() : Command( "geoWalk" ) {}
        virtual LockType locktype() const { return READ; }
        bool slaveOk() const { return true; }
        bool slaveOverrideOk() { return true; }
        bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string ns = dbname + "." + cmdObj.firstElement().valuestr();

            NamespaceDetails * d = nsdetails( ns.c_str() );
            if ( ! d ) {
                errmsg = "can't find ns";
                return false;
            }

            int geoIdx = -1;
            {
                NamespaceDetails::IndexIterator ii = d->ii();
                while ( ii.more() ) {
                    IndexDetails& id = ii.next();
                    if ( id.getSpec().getTypeName() == GEO2DNAME ) {
                        if ( geoIdx >= 0 ) {
                            errmsg = "2 geo indexes :(";
                            return false;
                        }
                        geoIdx = ii.pos() - 1;
                    }
                }
            }

            if ( geoIdx < 0 ) {
                errmsg = "no geo index :(";
                return false;
            }


            IndexDetails& id = d->idx( geoIdx );
            Geo2dType * g = (Geo2dType*)id.getSpec().getType();
            assert( &id == g->getDetails() );

            int max = 100000;

            BtreeCursor c( d , geoIdx , id , BSONObj() , BSONObj() , true , 1 );
            while ( c.ok() && max-- ) {
                GeoHash h( c.currKey().firstElement() );
                int len;
                cout << "\t" << h.toString()
                     << "\t" << c.current()[g->_geo]
                     << "\t" << hex << h.getHash()
                     << "\t" << hex << ((long long*)c.currKey().firstElement().binData(len))[0]
                     << "\t" << c.current()["_id"]
                     << endl;
                c.advance();
            }

            return true;
        }

    } geoWalkCmd;
    class Box2 : public Box {
    public:
	    Box2( Point min , Point max ): Box(min, max) {
	    }
	    Box2(){}
	    void extend( Point poi ){
		    if ( poi._x < _min._x ){
			    _min = Point(poi._x, _min._y);
		    } else if (poi._x > _max._x ){
			    _max = Point(poi._x, _max._y);
		    }
		    if ( poi._y < _min._y ){
			    _min = Point(_min._x, poi._y);
		    } else if (poi._y > _max._y ){
			    _max = Point(_max._x, poi._y);
		    }
	    }
    };

    class GeoMarker {
    public:
	    GeoMarker( Point poi , BSONObj obj ): _poi(poi), _obj(obj) {
	    }
	    GeoMarker() {}
	    Point _poi;
	    BSONObj _obj;
    };


    const double MinLat = -85.05112878;
    const double MaxLat = 85.05112878;
    const double MinLng = -180;
    const double MaxLng = 180;

    inline Point point_to_projection( const Point& poi ) {
	    double x = poi._x;
	    double y = poi._y;
	    x = min(max(MinLng, x), MaxLng);
	    y = min(max(MinLat, y), MaxLat);
	     
	    x = ( poi._x + 180 ) / 360;
	    y = sin(y * M_PI / 180);
	    y = 0.5 - ::log((1+y)/(1-y)) / (4 * M_PI);
	    return Point(x, y);
    }

    inline Point projection_to_point( const Point& poi ) {
	    double x = poi._x;
	    double y = poi._y;
	    x = ( x - 0.5 ) * 360;
	    y = 90 - 360 * atan(exp((y - 0.5) * 2 * M_PI)) / M_PI;
	    return Point(x, y);
    }


    class ClusterBox : public Box {
    public:
        ClusterBox( Point min , Point max , double extendDisance ): Box(min, max), count(0), _extendDisance( extendDisance ) {
	}
        ClusterBox() {}
        void addPoint(Point poi, GeoMarker marker) {
		if (count == 0) {
			_bounds = Box2(poi, poi);
			_centerx = poi._x;
			_centery = poi._y;
			_marker = marker;
		}else{
			_bounds.extend(poi);
			_centerx = (poi._x + _centerx * count) / (count + 1);
			_centery = (poi._y + _centery * count) / (count + 1);
		}
		refreshBounds();
		count++;
        }

	void refreshBounds(){
		Point cen = point_to_projection( center() );
		_min = projection_to_point(Point(cen._x - _extendDisance, cen._y + _extendDisance));
		_max = projection_to_point(Point(cen._x + _extendDisance, cen._y - _extendDisance));
	}

	Point center(){
		return Point(_centerx, _centery);
	}

        BSONObj obj() {
		BSONObjBuilder b;
		BSONArrayBuilder arr;
		BSONArrayBuilder min;
		BSONArrayBuilder max;
		BSONArrayBuilder cen;
		min.append(_bounds._min._x);
		min.append(_bounds._min._y);
		max.append(_bounds._max._x);
		max.append(_bounds._max._y);

		arr.append(min.arr());
		arr.append(max.arr());

		b.append( "bounds" , arr.arr() );
		b.appendNumber( "count" , count );
		cen.append(_centerx);
		cen.append(_centery);
		b.append( "center" , cen.arr() );
		return b.obj();
	}

	long long  count;
	GeoMarker _marker;
	Box2 _bounds;
	double _extendDisance;
	double _centerx;
	double _centery;
    };

    class GeoClusterBrowse : public GeoBoxBrowse {
    public:
        GeoClusterBrowse( const Geo2dType * g , const BSONObj& box , BSONObj filter = BSONObj() , bool needCluster = true, double gridSize  = 5 ): GeoBoxBrowse( g , box , filter ), _needCluster(needCluster), _gridSize( gridSize ) {
		Point minPro = point_to_projection(_want._min);
		Point maxPro = point_to_projection(_want._max);
		_extendDisance = min(maxPro._x - minPro._x, minPro._y - maxPro._y);
		//log() << "dis:" << _extendDisance << " min:" << projection_to_point(minPro) << " max:" << projection_to_point(maxPro) << endl;
		_extendDisance = _extendDisance / gridSize;
	}
	Box box() {
		return _want;
        }
        void curToCluster() {
		// Do exact check
		vector< BSONObj > locs;
                _g->getKeys( currLoc().obj(), locs );
                for( vector< BSONObj >::iterator i = locs.begin(); i != locs.end(); ++i ) {
			Point poi( *i );
			if( _want.inside( poi ) ) {
				if( _needCluster ) {
					bool used = false;
					for ( vector< ClusterBox >::iterator j = _clusters.begin(); j != _clusters.end(); j++ ) { 
						ClusterBox& box = *j;
						if(box.inside(poi)){
							used = true;
							//log() << "box: " << box.center() << " count:" << box.count << endl;
							box.addPoint(poi, GeoMarker(poi, current()));
							//log() << "add point: " << poi << " center: " << box.center() << " count:" << box.count << endl;
							break;
						}
					}
					if (!used) {
						ClusterBox box(poi, poi, _extendDisance);
						box.addPoint(poi, GeoMarker(poi, current()));
						//log() << "add box point: " << poi << " min:" << box._min << " max:" << box._max << endl;
						_clusters.push_back(box);
					}
				} else {
					_markers.push_back(GeoMarker(poi, current()));
				}
			}
		}
        }
	bool _needCluster;
	double _gridSize;
	double _extendDisance;
        vector< ClusterBox > _clusters;
        vector< GeoMarker > _markers;
    };
    /**
     * 	box: [[],[]]
     * 	piece: [5, 5]
     * 	cluster: true
     *
     * results:
     * [{bounds: [[],[]], count: 2, markers: []}]
     *
     */

    class Geo2dClusterCmd : public Command {
    public:
        Geo2dClusterCmd() : Command( "geoCluster" ) {}
        virtual LockType locktype() const { return READ; }
        bool slaveOk() const { return true; }
        void help(stringstream& h) const { h << "http://www.mongodb.org/display/DOCS/Geospatial+Indexing#GeospatialIndexing-geoNearCommand"; }
        bool slaveOverrideOk() { return true; }
        bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string ns = dbname + "." + cmdObj.firstElement().valuestr();

            Timer t;

            NamespaceDetails * d = nsdetails( ns.c_str() );
            if ( ! d ) {
                errmsg = "can't find ns";
                return false;
            }

            vector<int> idxs;
            d->findIndexByType( GEO2DNAME , idxs );

            //if ( idxs.size() > 1 ) {
            //    errmsg = "more than 1 geo indexes :(";
            //    return false;
            //}

            if ( idxs.size() == 0 ) {
                errmsg = "no geo index :(";
                return false;
            }

            int geoIdx = idxs[0];

            result.append( "ns" , ns );

            IndexDetails& id = d->idx( geoIdx );
            Geo2dType * g = (Geo2dType*)id.getSpec().getType();
            assert( &id == g->getDetails() );

	    uassert( 14051 , "'box' has to take an object or array" , cmdObj["box"].isABSONObj() );
	    BSONObj filter;
	    if ( cmdObj["query"].type() == Object )
                filter = cmdObj["query"].embeddedObject();

	    bool needCluster = true;
	    if ( cmdObj["disableCluster"].trueValue() )
		    needCluster = false;
            double gridSize = 5;
            if ( cmdObj["gridSize"].isNumber() )
                gridSize = cmdObj["gridSize"].numberDouble();


            //shared_ptr<Cursor> cursor( new GeoClusterBrowse( g , cmdObj["box"].embeddedObjectUserCheck() , filter ) );
            scoped_ptr<GeoClusterBrowse> cursor (new GeoClusterBrowse( g , cmdObj["box"].embeddedObjectUserCheck() , filter , needCluster, gridSize ));

	    //log() << "isNew " << newDoc << endl;
            while ( cursor->ok() ) {
		    cursor->curToCluster();
		    cursor->advance();
		    //RARELY killCurrentOp.checkForInterrupt();
	    }
	    vector< ClusterBox > clusters = cursor->_clusters;
	    vector< GeoMarker > markers = cursor->_markers;
            BSONArrayBuilder clusterarr( result.subarrayStart( "clusters" ) );
	    ClusterBox box;
	    for ( vector< ClusterBox >::iterator i = clusters.begin(); i != clusters.end(); i++ ) { 
	            box = *i;
		    if(box.count == 1){
			    markers.push_back(box._marker);
		    }else if( box.count > 0 ){
			    clusterarr.append(box.obj());
		    }
	    }
	    clusterarr.done();
            //BSONObjBuilder arr( result.subarrayStart( "results" ) );
            BSONArrayBuilder arr( result.subarrayStart( "markers" ) );
	    GeoMarker marker;
            int x = 0;
	    for ( vector< GeoMarker >::iterator i = markers.begin(); i != markers.end(); i++ ) { 
		    marker = *i;
		    BSONObjBuilder bb( arr.subobjStart( BSONObjBuilder::numStr( x++ ) ) );
		    BSONArrayBuilder pp( bb.subarrayStart( "point" ) );
		    pp.append(marker._poi._x);
		    pp.append(marker._poi._y);
		    pp.done();
		    bb.append( "obj" , marker._obj );
		    bb.done();
	    }
            arr.done();

            BSONObjBuilder stats( result.subobjStart( "stats" ) );
	    //stats.append( "time" , cc().curop()->elapsedMillis() );
	    stats.appendNumber( "timems" , t.millis() );
            //stats.appendNumber( "btreelocs" , gs._nscanned );
            stats.appendNumber( "nscanned" , cursor->nscanned() );
            //stats.appendNumber( "objectsLoaded" , gs._hopper->_objectsLoaded );
            //stats.append( "avgDistance" , totalDistance / x );
            //stats.append( "maxDistance" , gs._hopper->farthest() );
            //stats.done();
            return true;
        }

    } geo2dClusterCmd;

}
