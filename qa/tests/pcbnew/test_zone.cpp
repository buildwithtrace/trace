/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <pcbnew_utils/board_test_utils.h>

#include <board.h>
#include <footprint.h>
#include <geometry/shape_utils.h>
#include <zone.h>
#include <zone_utils.h>


struct ZONE_TEST_FIXTURE
{
    BOARD m_board;
};


static std::unique_ptr<ZONE> CreateSquareZone( BOARD_ITEM_CONTAINER& aParent, BOX2I aBox, PCB_LAYER_ID aLayer )
{
    auto zone = std::make_unique<ZONE>( &aParent );
    zone->SetLayer( aLayer );

    auto outline = std::make_unique<SHAPE_POLY_SET>();
    outline->AddOutline( KIGEOM::BoxToLineChain( aBox ) );

    zone->SetOutline( outline.release() );

    return zone;
}


/**
 * Create a similar zone (same outline) on a different layer
 */
static std::unique_ptr<ZONE> CreateSimilarZone( BOARD_ITEM_CONTAINER& aParent, const ZONE& aOther, PCB_LAYER_ID aLayer )
{
    auto zone = std::make_unique<ZONE>( &aParent );
    zone->SetLayer( aLayer );

    std::unique_ptr<SHAPE_POLY_SET> outline = std::make_unique<SHAPE_POLY_SET>( *aOther.Outline() );
    zone->SetOutline( outline.release() );

    return zone;
}


BOOST_FIXTURE_TEST_SUITE( Zone, ZONE_TEST_FIXTURE )

BOOST_AUTO_TEST_CASE( SingleLayer )
{
    ZONE zone( &m_board );

    zone.SetLayer( F_Cu );

    BOOST_TEST( zone.GetLayer() == F_Cu );
    BOOST_TEST( zone.GetLayer() == zone.GetFirstLayer() );

    BOOST_TEST( zone.IsOnCopperLayer() == true );
}

BOOST_AUTO_TEST_CASE( MultipleLayers )
{
    ZONE zone( &m_board );

    zone.SetLayerSet( { F_Cu, B_Cu } );

    // There is no "the" layer in a multi-layer zone
    BOOST_TEST( zone.GetLayer() == UNDEFINED_LAYER );
    // ... but there is a first layer
    BOOST_TEST( zone.GetFirstLayer() == F_Cu );

    BOOST_TEST( zone.IsOnCopperLayer() == true );
}

/**
 * During zone loading, the layer is set to Rescue if the layer is not found.
 * This is not a UI-visible layer, so make sure it can still be retreived.
 *
 * https://gitlab.com/kicad/code/kicad/-/issues/18553
 */
BOOST_AUTO_TEST_CASE( RescuedLayers )
{
    ZONE zone( &m_board );

    zone.SetLayer( Rescue );

    BOOST_TEST( zone.GetLayer() == Rescue );
    BOOST_TEST( zone.GetLayer() == zone.GetFirstLayer() );

    BOOST_TEST( zone.IsOnCopperLayer() == false );
}

/**
 * Verify that a rule area on all inner copper layers does not produce a
 * spurious layer validation error when the footprint uses the default
 * EXPAND_INNER_LAYERS stackup mode.
 *
 * Regression test for https://gitlab.com/kicad/code/kicad/-/issues/23042
 */
BOOST_AUTO_TEST_CASE( RuleAreaInnerLayersExpandMode )
{
    FOOTPRINT footprint( &m_board );
    footprint.SetStackupMode( FOOTPRINT_STACKUP::EXPAND_INNER_LAYERS );

    ZONE* ruleArea = new ZONE( &footprint );
    ruleArea->SetIsRuleArea( true );
    ruleArea->SetLayerSet( LSET::InternalCuMask() );
    footprint.Add( ruleArea );

    // Collect all layers used by the footprint (mirrors GetAllUsedFootprintLayers
    // from dialog_footprint_properties_fp_editor.cpp)
    LSET usedLayers;

    footprint.RunOnChildren(
            [&]( BOARD_ITEM* aItem )
            {
                if( aItem->Type() == PCB_ZONE_T )
                    usedLayers |= static_cast<ZONE*>( aItem )->GetLayerSet();
                else
                    usedLayers.set( aItem->GetLayer() );
            },
            RECURSE_MODE::RECURSE );

    // In EXPAND_INNER_LAYERS mode, F_Cu, B_Cu and all inner copper layers
    // are valid, along with tech, user, and user-defined layers.
    LSET allowedLayers = LSET{ F_Cu, B_Cu } | LSET::InternalCuMask();
    allowedLayers |= LSET::UserDefinedLayersMask( 4 );

    usedLayers &= ~allowedLayers;
    usedLayers &= ~LSET::AllTechMask();
    usedLayers &= ~LSET::UserMask();

    BOOST_TEST( usedLayers.none() );
}

/**
 * Verify that GetPosition() on a zone with no outline vertices does not
 * throw or crash. Empty zones can be created by importers.
 *
 * Regression test for https://gitlab.com/kicad/code/kicad/-/issues/23125
 */
BOOST_AUTO_TEST_CASE( EmptyZoneGetPosition )
{
    ZONE zone( &m_board );
    zone.SetLayer( F_Cu );

    BOOST_TEST( zone.GetNumCorners() == 0 );
    BOOST_CHECK_NO_THROW( zone.GetPosition() );
    BOOST_TEST( zone.GetPosition() == VECTOR2I( 0, 0 ) );
}


BOOST_AUTO_TEST_CASE( ZoneMergeNull )
{
    std::vector<std::unique_ptr<ZONE>> zones;

    zones.emplace_back( std::make_unique<ZONE>( &m_board ) );
    zones.back()->SetLayer( F_Cu );

    zones.emplace_back( std::make_unique<ZONE>( &m_board ) );
    zones.back()->SetLayer( F_Cu );

    std::vector<std::unique_ptr<ZONE>> merged = MergeZonesWithSameOutline( std::move( zones ) );

    // They are the same, so they do merge
    BOOST_TEST( merged.size() == 1 );
}


BOOST_AUTO_TEST_CASE( ZoneMergeNonNullNoMerge )
{
    std::vector<std::unique_ptr<ZONE>> zones;

    zones.emplace_back( CreateSquareZone( m_board, BOX2I( VECTOR2I( 0, 0 ), VECTOR2I( 100, 100 ) ), F_Cu ) );
    zones.emplace_back( CreateSquareZone( m_board, BOX2I( VECTOR2I( 200, 200 ), VECTOR2I( 300, 300 ) ), B_Cu ) );

    std::vector<std::unique_ptr<ZONE>> merged = MergeZonesWithSameOutline( std::move( zones ) );

    // They are different, so they don't merge
    BOOST_TEST( merged.size() == 2 );
}


BOOST_AUTO_TEST_CASE( ZoneMergeNonNullMerge )
{
    std::vector<std::unique_ptr<ZONE>> zones;

    zones.emplace_back( CreateSquareZone( m_board, BOX2I( VECTOR2I( 0, 0 ), VECTOR2I( 100, 100 ) ), F_Cu ) );
    zones.emplace_back( CreateSimilarZone( m_board, *zones.back(), B_Cu ) );

    std::vector<std::unique_ptr<ZONE>> merged = MergeZonesWithSameOutline( std::move( zones ) );

    // They are the same, so they do merge
    BOOST_REQUIRE( merged.size() == 1 );

    BOOST_TEST( merged[0]->GetLayerSet() == ( LSET{ F_Cu, B_Cu } ) );
    BOOST_TEST( merged[0]->GetNumCorners() == 4 );
}


BOOST_AUTO_TEST_CASE( ZoneMergeMergeSameGeomDifferentOrder )
{
    std::vector<std::unique_ptr<ZONE>> zones;

    zones.emplace_back( CreateSquareZone( m_board, BOX2I( VECTOR2I( 0, 0 ), VECTOR2I( 100, 100 ) ), F_Cu ) );
    zones.emplace_back( CreateSimilarZone( m_board, *zones.back(), B_Cu ) );

    // Reverse the outline of one of them
    // Don't go overboard here - detailed tests of CompareGeometry
    // should be in the SHAPE_LINE_CHAIN tests.
    auto newPolyB = std::make_unique<SHAPE_POLY_SET>( *zones.back()->Outline() );
    newPolyB->Outline( 0 ).Reverse();
    zones.back()->SetOutline( newPolyB.release() );

    std::vector<std::unique_ptr<ZONE>> merged = MergeZonesWithSameOutline( std::move( zones ) );

    // They are the same, so they do merge
    BOOST_REQUIRE( merged.size() == 1 );

    BOOST_TEST( merged[0]->GetLayerSet() == LSET( { F_Cu, B_Cu } ) );
    BOOST_TEST( merged[0]->GetNumCorners() == 4 );
}

BOOST_AUTO_TEST_SUITE_END()
