#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cata_catch.h"
#include "character.h"
#include "mutation.h"
#include "npc.h"
#include "player_helpers.h"
#include "type_id.h"

static const morale_type morale_perm_debug( "morale_perm_debug" );

static const mutation_category_id mutation_category_ALPHA( "ALPHA" );
static const mutation_category_id mutation_category_CHIMERA( "CHIMERA" );
static const mutation_category_id mutation_category_FELINE( "FELINE" );
static const mutation_category_id mutation_category_LUPINE( "LUPINE" );
static const mutation_category_id mutation_category_MOUSE( "MOUSE" );
static const mutation_category_id mutation_category_RAPTOR( "RAPTOR" );

static const trait_id trait_EAGLEEYED( "EAGLEEYED" );
static const trait_id trait_GOURMAND( "GOURMAND" );
static const trait_id trait_SMELLY( "SMELLY" );
static const trait_id trait_TEST_TRIGGER( "TEST_TRIGGER" );
static const trait_id trait_TEST_TRIGGER_2( "TEST_TRIGGER_2" );
static const trait_id trait_TEST_TRIGGER_2_active( "TEST_TRIGGER_2_active" );
static const trait_id trait_TEST_TRIGGER_active( "TEST_TRIGGER_active" );
static const trait_id trait_UGLY( "UGLY" );
static const trait_id trait_UNOBSERVANT( "UNOBSERVANT" );

static std::string get_mutations_as_string( const Character &you );

static mutation_category_id get_highest_category( const Character &you )
{
    int iLevel = 0;
    mutation_category_id sMaxCat;

    for( const std::pair<const mutation_category_id, int> &elem : you.mutation_category_level ) {
        if( elem.second > iLevel ) {
            sMaxCat = elem.first;
            iLevel = elem.second;
        } else if( elem.second == iLevel ) {
            sMaxCat = mutation_category_id();  // no category on ties
        }
    }
    return sMaxCat;
}

// Returns the list of mutations a player has as a string, for debugging
std::string get_mutations_as_string( const Character &you )
{
    std::ostringstream s;
    for( trait_id &m : you.get_mutations() ) {
        s << static_cast<std::string>( m ) << " ";
    }
    return s.str();
}

// Mutation categories may share common mutations. As a character accumulates mutations within a
// category, the chance of breaching threshold for that category increases.
//
// For example the SMELLY mutation is common to FELINE / LUPINE / MOUSE. A character with only the
// SMELLY mutation would be equally strong in all three categories, with an equal chance to breach
// any of them.
//
// The UGLY mutation is common to the FELINE / LUPINE / RAPTOR categories. A character having both
// SMELLY and UGLY would have their FELINE and LUPINE categories strengthened (since they have two
// mutations in those categories), relative to the MOUSE and RAPTOR categories.
//
// Adding GOURMAND, which is shared by LUPINE / MOUSE, should strengthen the character's LUPINE
// category further, and increase the chance to breach that category. If our character has all three
// mutations, their relative category strengths will look like this:
//
// RAPTOR: 1  (ugly)
// MOUSE:  2  (smelly. gourmand)
// FELINE: 2  (smelly, ugly)
// LUPINE: 3  (smelly, ugly, gourmand)
//
// This test illustrates and verifies the above scenario, using the same categories and mutations.
//
TEST_CASE( "mutation category strength based on current mutations", "[mutations][category]" )
{
    npc dummy;

    // With no mutations, no category is the highest
    CHECK( get_highest_category( dummy ).str().empty() );
    // All categories are at level 0
    CHECK( dummy.mutation_category_level[mutation_category_LUPINE] == 0 );
    CHECK( dummy.mutation_category_level[mutation_category_FELINE] == 0 );
    CHECK( dummy.mutation_category_level[mutation_category_RAPTOR] == 0 );
    CHECK( dummy.mutation_category_level[mutation_category_MOUSE] == 0 );

    // SMELLY mutation: Common to LUPINE, FELINE, and MOUSE
    REQUIRE( dummy.mutate_towards( trait_SMELLY ) );
    // No category should be highest
    CHECK( get_highest_category( dummy ).str().empty() );
    // All levels should be equal
    CHECK( dummy.mutation_category_level[mutation_category_LUPINE] ==
           dummy.mutation_category_level[mutation_category_FELINE] );
    CHECK( dummy.mutation_category_level[mutation_category_LUPINE] ==
           dummy.mutation_category_level[mutation_category_MOUSE] );

    // UGLY mutation: Common to LUPINE, FELINE, and RAPTOR
    REQUIRE( dummy.mutate_towards( trait_UGLY ) );
    // Still no highest, since LUPINE and FELINE should be tied
    CHECK( get_highest_category( dummy ).str().empty() );
    // LUPINE and FELINE should be equal level, and both stronger than MOUSE or RAPTOR
    CHECK( dummy.mutation_category_level[mutation_category_LUPINE] ==
           dummy.mutation_category_level[mutation_category_FELINE] );
    CHECK( dummy.mutation_category_level[mutation_category_LUPINE] >
           dummy.mutation_category_level[mutation_category_MOUSE] );
    CHECK( dummy.mutation_category_level[mutation_category_LUPINE] >
           dummy.mutation_category_level[mutation_category_RAPTOR] );

    // GROWL mutation: Common to LUPINE and MOUSE
    REQUIRE( dummy.mutate_towards( trait_GOURMAND ) );
    // LUPINE has the most mutations now, and should now be the strongest category
    CHECK( get_highest_category( dummy ).str() == "LUPINE" );
    // LUPINE category level should be strictly higher than any other
    CHECK( dummy.mutation_category_level[mutation_category_LUPINE] >
           dummy.mutation_category_level[mutation_category_FELINE] );
    CHECK( dummy.mutation_category_level[mutation_category_LUPINE] >
           dummy.mutation_category_level[mutation_category_MOUSE] );
    CHECK( dummy.mutation_category_level[mutation_category_LUPINE] >
           dummy.mutation_category_level[mutation_category_RAPTOR] );
}

// If character has all available mutations in a category (pre- or post-threshold), that should be
// their strongest/highest category. This test verifies that expectation for every category.
TEST_CASE( "Having all mutations give correct highest category", "[mutations][strongest]" )
{
    for( const std::pair<const mutation_category_id, mutation_category_trait> &cat :
         mutation_category_trait::get_all() ) {
        const mutation_category_trait &cur_cat = cat.second;
        const std::string cat_str = cur_cat.id.str();
        if( cat_str == "ANY" ) {
            continue;
        }
        // Unfinished mutation category.
        if( cur_cat.wip ) {
            continue;
        }

        GIVEN( "The player has all pre-threshold mutations for " + cat_str ) {
            npc dummy;
            dummy.set_body();
            dummy.give_all_mutations( cur_cat, false );

            THEN( cat_str + " is the strongest category" ) {
                INFO( "MUTATIONS: " << get_mutations_as_string( dummy ) );
                CHECK( get_highest_category( dummy ).str() == cat_str );
            }
        }

        GIVEN( "The player has all mutations for " + cat_str ) {
            npc dummy;
            dummy.set_body();
            dummy.give_all_mutations( cur_cat, true );

            THEN( cat_str + " is the strongest category" ) {
                INFO( "MUTATIONS: " << get_mutations_as_string( dummy ) );
                CHECK( get_highest_category( dummy ).str() == cat_str );
            }
        }
    }
}

// If character has all the pre-threshold mutations for a category, they should have a chance of
// breaching the threshold on mutation. The chance of breach is expected to be between 55% and 90%
// given that the breach power is rolled out of 100.  In addition, a power below 30 is ignored.
//
// If a category breach power falls below 55, it suggests that category lacks enough pre-threshold mutations
// to comfortably cross the Threshold
// If a category breach power goes above 90, it suggests that category has too many pre-threshold mutations
// which suggests that some should be moved to post-threshold
//
// When creating or editing a category, remember that 55 and 90 are limits, not suggestions
// 65-75 is the suggested range
//
// This test verifies the breach-power expectation for all mutation categories.
TEST_CASE( "Having all pre-threshold mutations gives a sensible threshold breach power",
           "[mutations][breach]" )
{
    const int BREACH_POWER_MIN = 55;
    const int BREACH_POWER_MAX = 90;

    for( const std::pair<mutation_category_id, mutation_category_trait> cat :
         mutation_category_trait::get_all() ) {
        const mutation_category_trait &cur_cat = cat.second;
        const mutation_category_id &cat_id = cur_cat.id;
        if( cur_cat.threshold_mut.is_empty() ) {
            continue;
        }
        // Unfinished mutation category.
        if( cur_cat.wip ) {
            continue;
        }

        GIVEN( "The player has all pre-threshold mutations for " + cat_id.str() ) {
            npc dummy;
            dummy.set_body();
            dummy.give_all_mutations( cur_cat, false );

            const int breach_chance = dummy.mutation_category_level[cat_id];
            if( cat_id == mutation_category_ALPHA ) {
                THEN( "Alpha Threshold breach power is between 35 and 60" ) {
                    INFO( "MUTATIONS: " << get_mutations_as_string( dummy ) );
                    CHECK( breach_chance >= 35 );
                    CHECK( breach_chance <= 60 );
                }
                continue;
            } else if( cat_id == mutation_category_CHIMERA ) {
                THEN( "Chimera Threshold breach power is between 100 and 160" ) {
                    INFO( "MUTATIONS: " << get_mutations_as_string( dummy ) );
                    CHECK( breach_chance >= 100 );
                    CHECK( breach_chance <= 160 );
                }
                continue;
            }
            THEN( "Threshold breach power is between 55 and 90" ) {
                INFO( "MUTATIONS: " << get_mutations_as_string( dummy ) );
                CHECK( breach_chance >= BREACH_POWER_MIN );
                CHECK( breach_chance <= BREACH_POWER_MAX );
            }
        }
    }
}

TEST_CASE( "Scout and Topographagnosia traits affect overmap sight range", "[mutations][overmap]" )
{
    Character &dummy = get_player_character();
    clear_avatar();

    WHEN( "character has Scout trait" ) {
        dummy.toggle_trait( trait_EAGLEEYED );
        THEN( "they have increased overmap sight range" ) {
            CHECK( dummy.mutation_value( "overmap_sight" ) == 5 );
        }
        // Regression test for #42853
        THEN( "having another trait does not cancel the Scout trait" ) {
            dummy.toggle_trait( trait_SMELLY );
            CHECK( dummy.mutation_value( "overmap_sight" ) == 5 );
        }
    }

    WHEN( "character has Topographagnosia trait" ) {
        dummy.toggle_trait( trait_UNOBSERVANT );
        THEN( "they have reduced overmap sight range" ) {
            CHECK( dummy.mutation_value( "overmap_sight" ) == -10 );
        }
        // Regression test for #42853
        THEN( "having another trait does not cancel the Topographagnosia trait" ) {
            dummy.toggle_trait( trait_SMELLY );
            CHECK( dummy.mutation_value( "overmap_sight" ) == -10 );
        }
    }
}

static void check_test_mutation_is_triggered( const Character &dummy, bool trigger_on )
{
    if( trigger_on ) {
        THEN( "the mutation turns on" ) {
            CHECK( dummy.has_trait( trait_TEST_TRIGGER_active ) );
            CHECK( !dummy.has_trait( trait_TEST_TRIGGER ) );
        }
    } else {
        THEN( "the mutation turns off" ) {
            CHECK( !dummy.has_trait( trait_TEST_TRIGGER_active ) );
            CHECK( dummy.has_trait( trait_TEST_TRIGGER ) );
        }
    }
}


TEST_CASE( "The various type of triggers work", "[mutations]" )
{
    Character &dummy = get_player_character();
    clear_avatar();

    WHEN( "character has OR test trigger mutation" ) {
        dummy.toggle_trait( trait_TEST_TRIGGER );

        WHEN( "character is happy" ) {
            dummy.add_morale( morale_perm_debug, 21 );
            dummy.apply_persistent_morale();
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, true );
        }

        WHEN( "character is no longer happy" ) {
            dummy.clear_morale();
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, false );
        }

        WHEN( "it is the full moon" ) {
            static const time_point full_moon = calendar::turn_zero + calendar::season_length() / 6;
            calendar::turn = full_moon;
            INFO( "MOON PHASE : " << io::enum_to_string<moon_phase>( get_moon_phase( calendar::turn ) ) );
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, true );
        }

        WHEN( "it's no longer the full moon" ) {
            calendar::turn = calendar::turn_zero;
            INFO( "MOON PHASE : " << io::enum_to_string<moon_phase>( get_moon_phase( calendar::turn ) ) );
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, false );
        }

        WHEN( "character is very hungry" ) {
            dummy.set_hunger( 120 );
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, true );
        }

        WHEN( "character is no longer very hungry" ) {
            dummy.set_hunger( 0 );
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, false );
        }

        WHEN( "character is in pain" ) {
            dummy.set_pain( 120 );
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, true );
        }

        WHEN( "character is no longer in pain" ) {
            dummy.set_pain( 0 );
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, false );
        }

        WHEN( "character is thirsty" ) {
            dummy.set_thirst( 120 );
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, true );
        }

        WHEN( "character is no longer thirsty" ) {
            dummy.set_thirst( 0 );
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, false );
        }

        WHEN( "character is low on stamina" ) {
            dummy.set_stamina( 0 );
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, true );
        }

        WHEN( "character is no longer low on stamina " ) {
            dummy.set_stamina( dummy.get_stamina_max() );
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, false );
        }

        WHEN( "it's 2 am" ) {
            calendar::turn = calendar::turn_zero + 2_hours;
            INFO( "TIME OF DAY : " << to_string_time_of_day( calendar::turn ) );
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, true );
        }

        WHEN( "it's no longer 2 am" ) {
            calendar::turn = calendar::turn_zero;
            INFO( "TIME OF DAY : " << to_string_time_of_day( calendar::turn ) );
            dummy.process_turn();
            check_test_mutation_is_triggered( dummy, false );
        }
    }

    clear_avatar();

    WHEN( "character has AND test trigger mutation" ) {
        dummy.toggle_trait( trait_TEST_TRIGGER_2 );

        WHEN( "it is the full moon but character is not in pain" ) {
            static const time_point full_moon = calendar::turn_zero + calendar::season_length() / 6;
            calendar::turn = full_moon;
            INFO( "MOON PHASE : " << io::enum_to_string<moon_phase>( get_moon_phase( calendar::turn ) ) );
            dummy.process_turn();

            THEN( "the mutation stays turned off" ) {
                CHECK( !dummy.has_trait( trait_TEST_TRIGGER_2_active ) );
                CHECK( dummy.has_trait( trait_TEST_TRIGGER_2 ) );
            }
        }

        WHEN( "character is in pain and it's the full moon" ) {
            dummy.set_pain( 120 );
            dummy.process_turn();

            THEN( "the mutation turns on" ) {
                CHECK( dummy.has_trait( trait_TEST_TRIGGER_2_active ) );
                CHECK( !dummy.has_trait( trait_TEST_TRIGGER_2 ) );
            }
        }

        WHEN( "character is no longer in pain" ) {
            dummy.set_pain( 0 );
            dummy.process_turn();

            THEN( "the mutation turns off" ) {
                CHECK( !dummy.has_trait( trait_TEST_TRIGGER_2_active ) );
                CHECK( dummy.has_trait( trait_TEST_TRIGGER_2 ) );
            }
        }
    }

}
