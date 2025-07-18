#include "auto_pickup.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iosfwd>
#include <map>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "cata_path.h"
#include "cata_utility.h"
#include "character.h"
#include "color.h"
#include "coordinates.h"
#include "cursesdef.h"
#include "filesystem.h"
#include "flag.h"
#include "flat_set.h"
#include "flexbuffer_json.h"
#include "input_context.h"
#include "input_popup.h"
#include "item.h"
#include "item_factory.h"
#include "item_location.h"
#include "item_stack.h"
#include "itype.h"
#include "json.h"
#include "map.h"
#include "map_selector.h"
#include "material.h"
#include "options.h"
#include "output.h"
#include "path_info.h"
#include "point.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"
#include "uilist.h"
#include "ui_manager.h"
#include "units.h"

using namespace auto_pickup;

static const ammotype ammo_battery( "battery" );

static bool check_special_rule( const std::map<material_id, int> &materials,
                                std::string_view rule );

auto_pickup::player_settings &get_auto_pickup()
{
    static auto_pickup::player_settings single_instance;
    return single_instance;
}

/**
 * The function will return `true` if the user has set all limits to a value of 0.
 * @param pickup_item item to check.
 * @return `true` if given item's weight and volume is within auto pickup user configured limits.
 */
static bool within_autopickup_limits( const item *pickup_item )
{
    bool valid_item = !pickup_item->has_any_flag( cata::flat_set<flag_id> { flag_ZERO_WEIGHT, flag_NO_DROP } );

    int weight_limit = get_option<int>( "AUTO_PICKUP_WEIGHT_LIMIT" );
    int volume_limit = get_option<int>( "AUTO_PICKUP_VOLUME_LIMIT" );

    bool valid_volume = pickup_item->volume() <= volume_limit * 50_ml;
    bool valid_weight = pickup_item->weight() <= weight_limit * 50_gram;

    return valid_item && ( volume_limit <= 0 || valid_volume ) && ( weight_limit <= 0 || valid_weight );
}

/**
 * @param pickup_item item to get the auto pickup rule for.
 * @return `rule_state` associated with the given item.
 */
static rule_state get_autopickup_rule( const item *pickup_item )
{
    std::string item_name = pickup_item->tname( 1, false );
    rule_state pickup_state = get_auto_pickup().check_item( item_name );

    if( pickup_state == rule_state::WHITELISTED ) {
        return rule_state::WHITELISTED;
    } else if( pickup_state != rule_state::BLACKLISTED ) {
        //No prematched pickup rule found, check rules in more detail
        get_auto_pickup().create_rule( pickup_item );

        if( get_auto_pickup().check_item( item_name ) == rule_state::WHITELISTED ) {
            return rule_state::WHITELISTED;
        }
    } else {
        return rule_state::BLACKLISTED;
    }
    return rule_state::NONE;
}

/**
 * Drop all items from the given container that match special auto pickup rules.
 * The items will be removed from the container and dropped in the designated location.
 *
 * @param from container to drop items from.
 * @param where location on the map to drop items to.
 */
static void empty_autopickup_target( item *what, tripoint_bub_ms where )
{
    bool is_rigid = what->all_pockets_rigid();
    for( item *entry : what->all_items_top() ) {
        const rule_state ap_rule = get_autopickup_rule( entry );
        // rigid containers want to keep as much items as possible so drop only blacklisted items
        // non-rigid containers want to drop as much items as possible so keep only whitelisted items
        if( is_rigid ? ap_rule == rule_state::BLACKLISTED : ap_rule != rule_state::WHITELISTED ) {
            // drop the items on the same tile the container is on
            get_map().add_item( where, what->remove_item( *entry ) );
        }
    }
}

/**
 * Iterate through every item inside the container to find items that match auto pickup rules.
 * In most cases whitelisted items will be included and blacklisted one will be excluded however
 * there are special cases. Below is an overview of selection rules for container auto pickup.
 *
 * Containers and items will **never** be picked up when:
 *
 * - they are owned by player and `AUTO_PICKUP_OWNED` game option is disabled.
 * - they exceed volume or weight user auto pickup limitations.
 * - they are blacklisted in auto pickup rules.
 *
 * Containers will **always** be picked up when:
 *
 * - the container is sealed.
 * - there is any liquids stored in container pockets.
 * - all items inside the container are marked for pickup.
 *
 * Containers will **never** be picked up when:
 *
 * - only batteries were selected and the container is battery powered.
 * - the container is a non-whitelisted corpse.
 *
 * Items will **NOT** be picked up when:
 *
 * - the parent container is non-rigid and the item is not whitelisted.
 *
 * @param from item to search for items to auto pickup from.
 * @return sequence of items to auto pickup from given container.
 */
static std::vector<item_location> get_autopickup_items( item_location &from )
{
    map &here = get_map();

    item *container_item = from.get_item();
    // items sealed in containers should never be unsealed by auto pickup
    bool force_pick_container = container_item->any_pockets_sealed();
    bool pick_all_items = true;

    std::vector<item_location> result;
    // do not auto pickup owned containers or items
    if( !get_option<bool>( "AUTO_PICKUP_OWNED" ) &&
        container_item->is_owned_by( get_player_character() ) ) {
        return result;
    }
    std::list<item *> contents = container_item->all_items_top();
    result.reserve( contents.size() );

    bool any_whitelisted = false;
    std::list<item *>::iterator it;
    for( it = contents.begin(); it != contents.end(); ++it ) {
        item *item_entry = *it;
        if( !within_autopickup_limits( item_entry ) ) {
            pick_all_items = false;
            continue;
        }
        const rule_state pickup_state = get_autopickup_rule( item_entry );
        if( pickup_state == rule_state::WHITELISTED ) {
            any_whitelisted = true;
            if( !force_pick_container ) {
                if( item_entry->is_container() ) {
                    // whitelisted containers should exclude contained blacklisted items
                    empty_autopickup_target( item_entry, from.pos_bub( here ) );
                } else if( item_entry->made_of_from_type( phase_id::LIQUID ) ) {
                    // liquid items should never be picked up without container
                    force_pick_container = true;
                    break;
                }
                // pick up the whitelisted item
                result.emplace_back( from, item_entry );
            }
        } else if( !force_pick_container && item_entry->is_container() &&
                   !item_entry->is_container_empty() ) {
            // get pickup list from nested item container
            item_location location = item_location( from, item_entry );
            std::vector<item_location> result_nested = get_autopickup_items( location );

            // container with content was NOT marked for pickup
            if( result_nested.size() != 1 || result_nested[0] != location ) {
                pick_all_items = false;
            }
            result.reserve( result_nested.size() + result.size() );
            result.insert( result.end(), result_nested.begin(), result_nested.end() );
        } else {
            // skip not whitelisted items that are not containers with items
            pick_all_items = false;
        }
    }
    // all items in container were approved for pickup
    if( !contents.empty() && ( pick_all_items || force_pick_container ) ) {
        // only auto pickup corpses if they are whitelisted
        // blacklisted containers should still have their contents picked up but themselves should be excluded.
        // If all items inside blacklisted container match then just pickup the items without the container
        rule_state pickup_state = get_autopickup_rule( container_item );
        if( pickup_state == rule_state::BLACKLISTED || !any_whitelisted ||
            ( container_item->is_corpse() && pickup_state != rule_state::WHITELISTED ) ) {
            return result;
        }
        bool all_batteries = true;
        bool powered_container = container_item->ammo_capacity( ammo_battery );
        if( powered_container ) {
            // when dealing with battery powered tools there should only be one pocket
            // and one battery inside but this could change in future so account for that here
            all_batteries = std::all_of( result.begin(), result.end(),
            []( const item_location & il ) {
                return il.get_item()->is_battery();
            } );
        }
        bool batteries_from_tool = powered_container && all_batteries;
        // make sure container is allowed to be picked up
        // when picking up batteries from powered containers don't pick container
        if( within_autopickup_limits( container_item ) && !batteries_from_tool ) {
            result.clear();
            result.push_back( from );
        } else if( force_pick_container ) {
            // when force picking never pick individual items
            result.clear();
        }
    }
    return result;
}

/**
 * Select which items on the map tile should be auto-picked up.
 * The return value represents locations of the selected items on the map.
 *
 * @param from stack of item entries on a map tile.
 * @param location where the stack of items is located on the map.
 * @return sequence of selected items on the map.
 */
drop_locations auto_pickup::select_items(
    const std::vector<item_stack::iterator> &from, const tripoint_bub_ms &location )
{
    drop_locations result;
    const map_cursor map_location = map_cursor( location );

    // iterate over all item stacks found in location
    for( const item_stack::iterator &stack : from ) {
        item *item_entry = &*stack;
        // do not auto pickup owned containers or items
        if( !get_option<bool>( "AUTO_PICKUP_OWNED" ) &&
            item_entry->is_owned_by( get_player_character() ) ) {
            continue;
        }
        // do not auto pickup spilt liquids
        if( item_entry->made_of( phase_id::LIQUID ) ) {
            continue;
        }
        rule_state pickup_state = get_autopickup_rule( item_entry );
        bool is_container = item_entry->is_container() && !item_entry->empty_container();

        // before checking contents check if item is on pickup list
        if( pickup_state == rule_state::WHITELISTED ) {
            if( item_entry->is_container() ) {
                empty_autopickup_target( item_entry, location );
            }
            // skip if the container is still above the limit after emptying it
            if( !within_autopickup_limits( item_entry ) ) {
                continue;
            }
            int it_count = 0; // TODO: factor in auto pickup max_quantity here
            item_location it_location = item_location( map_location, item_entry );
            result.emplace_back( std::make_pair( it_location, it_count ) );
        } else if( is_container || item_entry->ammo_capacity( ammo_battery ) ) {
            item_location container_location = item_location( map_location, item_entry );
            for( const item_location &add_item : get_autopickup_items( container_location ) ) {
                int it_count = 0; // TODO: factor in auto pickup max_quantity here
                result.emplace_back( std::make_pair( add_item, it_count ) );
            }
        }
    }
    return result;
}

void user_interface::show()
{
    if( tabs.empty() ) {
        return;
    }

    const int iHeaderHeight = 4;
    int iContentHeight = 0;

    catacurses::window w_border;
    catacurses::window w_header;
    catacurses::window w;

    ui_adaptor ui;

    const auto init_windows = [&]( ui_adaptor & ui ) {
        iContentHeight = FULL_SCREEN_HEIGHT - 2 - iHeaderHeight;
        const point iOffset( TERMX > FULL_SCREEN_WIDTH ? ( TERMX - FULL_SCREEN_WIDTH ) / 2 : 0,
                             TERMY > FULL_SCREEN_HEIGHT ? ( TERMY - FULL_SCREEN_HEIGHT ) / 2 : 0 );

        w_border = catacurses::newwin( FULL_SCREEN_HEIGHT, FULL_SCREEN_WIDTH,
                                       iOffset );
        w_header = catacurses::newwin( iHeaderHeight, FULL_SCREEN_WIDTH - 2,
                                       iOffset + point::south_east );
        w = catacurses::newwin( iContentHeight, FULL_SCREEN_WIDTH - 2,
                                iOffset + point( 1, iHeaderHeight + 1 ) );

        ui.position_from_window( w_border );
    };
    init_windows( ui );
    ui.on_screen_resize( init_windows );

    size_t iTab = 0;
    int iLine = 0;
    bool bLeftColumn = true;
    int iStartPos = 0;
    Character &player_character = get_player_character();
    bStuffChanged = false;
    input_context ctxt( "AUTO_PICKUP" );
    ctxt.register_navigate_ui_list();
    ctxt.register_leftright();
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    if( tabs.size() > 1 ) {
        ctxt.register_action( "NEXT_TAB" );
        ctxt.register_action( "PREV_TAB" );
    }
    ctxt.register_action( "ADD_RULE" );
    ctxt.register_action( "REMOVE_RULE" );
    ctxt.register_action( "COPY_RULE" );
    ctxt.register_action( "ENABLE_RULE" );
    ctxt.register_action( "DISABLE_RULE" );
    ctxt.register_action( "MOVE_RULE_UP" );
    ctxt.register_action( "MOVE_RULE_DOWN" );
    ctxt.register_action( "TEST_RULE" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "SWITCH_AUTO_PICKUP_OPTION" );
    const bool allow_swapping = tabs.size() == 2;
    if( allow_swapping ) {
        ctxt.register_action( "SWAP_RULE_GLOBAL_CHAR" );
    }

    ui.on_redraw( [&]( const ui_adaptor & ) {
        // Redraw the border
        draw_border( w_border, BORDER_COLOR, title );

        wattron( w, c_light_gray );
        // |-
        mvwaddch( w_border, point( 0, 3 ), LINE_XXXO );
        // -|
        mvwaddch( w_border, point( 79, 3 ), LINE_XOXX );
        // _|_
        mvwaddch( w_border, point( 5, FULL_SCREEN_HEIGHT - 1 ), LINE_XXOX );
        mvwaddch( w_border, point( 51, FULL_SCREEN_HEIGHT - 1 ), LINE_XXOX );
        mvwaddch( w_border, point( 61, FULL_SCREEN_HEIGHT - 1 ), LINE_XXOX );
        wattroff( w, c_light_gray );
        wnoutrefresh( w_border );

        // Redraw the header
        // NOLINTNEXTLINE(cata-use-named-point-constants)
        mvwhline( w_header, point( 0, 0 ), ' ', FULL_SCREEN_WIDTH - 2 );  // clear the line
        const bool enabled = get_option<bool>( "AUTO_PICKUP" );
        nc_color color = c_white;
        // NOLINTNEXTLINE(cata-use-named-point-constants)
        print_colored_text( w_header, point( 1, 0 ), color, c_white, string_format( "%s %s",
                            ctxt.get_desc( "SWITCH_AUTO_PICKUP_OPTION", _( "Auto pickup enabled:" ) ),
                            colorize( enabled ? _( "True" ) : _( "False" ),
                                      enabled ? c_light_green : c_light_red ) ) );
        // NOLINTNEXTLINE(cata-use-named-point-constants)
        print_colored_text( w_header, point( 1, 1 ), color, c_white,
                            ctxt.get_desc( "HELP_KEYBINDINGS", _( "Display keybindings" ) ) );

        wattron( w_header, c_light_gray );
        mvwhline( w_header, point( 0,  2 ), LINE_OXOX, 78 );
        for( int x : {
                 4, 50, 60
             } ) {
            mvwaddch( w_header, point( x, 2 ), LINE_OXXX );
            mvwaddch( w_header, point( x, 3 ), LINE_XOXO );
        }
        wattroff( w_header, c_light_gray );
        mvwprintz( w_header, point( 1, 3 ), c_white, "#" );
        mvwprintz( w_header, point( 8, 3 ), c_white, _( "Rules" ) );
        mvwprintz( w_header, point( 52, 3 ), c_white, _( "Inc/Exc" ) );

        rule_list &cur_rules = tabs[iTab].new_rules;
        int locx = 17;
        for( size_t i = 0; i < tabs.size(); i++ ) {
            const nc_color color = iTab == i ? hilite( c_white ) : c_white;
            locx += shortcut_print( w_header, point( locx, 2 ), c_white, color, tabs[i].title ) + 1;
        }

        wnoutrefresh( w_header );

        // Clear the lines
        mvwrectf( w, point::zero, c_black, ' ', 79, iContentHeight );
        for( int x : {
                 4, 50, 60
             } ) {
            mvwvline( w, point( x, 0 ), c_light_gray, LINE_XOXO, iContentHeight );
        }

        draw_scrollbar( w_border, iLine, iContentHeight, cur_rules.size(), point( 0, 5 ) );
        wnoutrefresh( w_border );

        calcStartPos( iStartPos, iLine, iContentHeight, cur_rules.size() );

        // display auto pickup
        for( int i = iStartPos; i < static_cast<int>( cur_rules.size() ); i++ ) {
            if( i >= iStartPos &&
                i < iStartPos + std::min<int>( iContentHeight, cur_rules.size() ) ) {
                nc_color cLineColor = cur_rules[i].bActive ? c_white : c_light_gray;

                mvwprintz( w, point( 1, i - iStartPos ), cLineColor, "%d", i + 1 );
                mvwprintz( w, point( 5, i - iStartPos ), cLineColor, "" );

                if( iLine == i ) {
                    wprintz( w, c_yellow, ">> " );
                } else {
                    wprintz( w, c_yellow, "   " );
                }

                wprintz( w, iLine == i && bLeftColumn ? hilite( cLineColor ) : cLineColor, "%s",
                         cur_rules[i].sRule.empty() ? _( "<empty rule>" ) : cur_rules[i].sRule );

                mvwprintz( w, point( 52, i - iStartPos ), iLine == i && !bLeftColumn ?
                           hilite( cLineColor ) : cLineColor, "%s",
                           cur_rules[i].bExclude ? _( "Exclude" ) :  _( "Include" ) );
            }
        }

        wnoutrefresh( w );
    } );

    while( true ) {
        rule_list &cur_rules = tabs[iTab].new_rules;

        const bool currentPageNonEmpty = !cur_rules.empty();

        ui_manager::redraw();

        const int recmax = static_cast<int>( cur_rules.size() );
        const int scroll_rate = recmax > 20 ? 10 : 3;
        const std::string action = ctxt.handle_input();

        if( action == "NEXT_TAB" || action == "PREV_TAB" ) {
            iTab = inc_clamp_wrap( iTab, action == "NEXT_TAB", tabs.size() );
            iLine = 0;
        } else if( action == "QUIT" ) {
            break;
        } else if( navigate_ui_list( action, iLine, scroll_rate, recmax, true ) ) {
        } else if( action == "REMOVE_RULE" && currentPageNonEmpty ) {
            bStuffChanged = true;
            cur_rules.erase( cur_rules.begin() + iLine );
            // after erase, recmax - 2 is the last valid index
            iLine = std::clamp( iLine, 0, recmax - 2 );
        } else if( action == "COPY_RULE" && currentPageNonEmpty ) {
            bStuffChanged = true;
            cur_rules.push_back( cur_rules[iLine] );
            iLine = cur_rules.size() - 1;
        } else if( allow_swapping && action == "SWAP_RULE_GLOBAL_CHAR" && currentPageNonEmpty ) {
            const size_t other_iTab = ( iTab + 1 ) % 2;
            rule_list &other_rules = tabs[other_iTab].new_rules;
            bStuffChanged = true;
            //copy over
            other_rules.push_back( cur_rules[iLine] );
            //remove old
            cur_rules.erase( cur_rules.begin() + iLine );
            iTab = other_iTab;
            iLine = other_rules.size() - 1;
        } else if( action == "ADD_RULE" || ( action == "CONFIRM" && currentPageNonEmpty ) ) {
            const int old_iLine = iLine;
            if( action == "ADD_RULE" ) {
                cur_rules.push_back( rule( "", true, false ) );
                iLine = cur_rules.size() - 1;
            }
            ui_manager::redraw();

            if( bLeftColumn || action == "ADD_RULE" ) {
                string_input_popup_imgui popup( 60, cur_rules[iLine].sRule );
                popup.set_label( _( "Pickup Rule:" ) );
                std::string description = _(
                                              "* is used as a Wildcard.  A few Examples:\n"
                                              "\n"
                                              "wooden arrow    matches the itemname exactly\n"
                                              "wooden ar*      matches items beginning with wood ar\n"
                                              "*rrow           matches items ending with rrow\n"
                                              "*avy fle*fi*arrow     multiple * are allowed\n"
                                              "heAVY*woOD*arrOW      case insensitive search\n"
                                              "\n"
                                              "Pickup based on item materials:\n"
                                              "m:kevlar        matches items made of Kevlar\n"
                                              "M:copper        matches items made purely of copper\n"
                                              "M:steel,iron    multiple materials allowed (OR search)"
                                          );
                popup.set_description( description, c_white, true );
                const std::string r = popup.query();
                // If r is empty, then either (1) The player ESC'ed from the window (changed their mind), or
                // (2) Explicitly entered an empty rule- which isn't allowed since "*" should be used
                // to include/exclude everything
                if( !r.empty() ) {
                    cur_rules[iLine].sRule = wildcard_trim_rule( r );
                    bStuffChanged = true;
                } else if( action == "ADD_RULE" ) {
                    cur_rules.pop_back();
                    iLine = old_iLine;
                }
            } else if( !bLeftColumn ) {
                bStuffChanged = true;
                cur_rules[iLine].bExclude = !cur_rules[iLine].bExclude;
            }
        } else if( action == "ENABLE_RULE" && currentPageNonEmpty ) {
            bStuffChanged = true;
            cur_rules[iLine].bActive = true;
        } else if( action == "DISABLE_RULE" && currentPageNonEmpty ) {
            bStuffChanged = true;
            cur_rules[iLine].bActive = false;
        } else if( action == "LEFT" || action == "RIGHT" ) {
            bLeftColumn = !bLeftColumn;
        } else if( action == "MOVE_RULE_UP" && currentPageNonEmpty ) {
            bStuffChanged = true;
            if( iLine < recmax - 1 ) {
                std::swap( cur_rules[iLine], cur_rules[iLine + 1] );
                iLine++;
            }
        } else if( action == "MOVE_RULE_DOWN" && currentPageNonEmpty ) {
            bStuffChanged = true;
            if( iLine > 0 ) {
                std::swap( cur_rules[iLine], cur_rules[iLine - 1] );
                iLine--;
            }
        } else if( action == "TEST_RULE" && currentPageNonEmpty && !player_character.name.empty() ) {
            cur_rules[iLine].test_pattern();
        } else if( action == "SWITCH_AUTO_PICKUP_OPTION" ) {
            get_options().get_option( "AUTO_PICKUP" ).setNext();
            get_options().save();
        }
    }

    if( !bStuffChanged ) {
        return;
    }

    if( !query_yn( _( "Save changes?" ) ) ) {
        return;
    }

    for( tab &t : tabs ) {
        t.rules.get() = t.new_rules;
    }
}

void player_settings::show()
{
    user_interface ui;

    Character &player_character = get_player_character();
    ui.title = _( "Auto pickup manager" );
    ui.tabs.emplace_back( _( "[<Global>]" ), global_rules );
    if( !player_character.name.empty() ) {
        ui.tabs.emplace_back( _( "[<Character>]" ), character_rules );
    }

    ui.show();

    if( !ui.bStuffChanged ) {
        return;
    }

    save_global();
    if( !player_character.name.empty() ) {
        save_character();
    }
    invalidate();
}

void rule::test_pattern() const
{
    std::vector<std::string> vMatchingItems;

    if( sRule.empty() ) {
        return;
    }

    //Loop through all itemfactory items
    //APU now ignores prefixes, bottled items and suffix combinations still not generated
    for( const itype *e : item_controller->all() ) {
        const std::string sItemName = e->nname( 1 );
        if( !check_special_rule( e->materials, sRule ) && !wildcard_match( sItemName, sRule ) ) {
            continue;
        }

        vMatchingItems.push_back( sItemName );
    }

    int iStartPos = 0;
    int iContentHeight = 0;
    int iContentWidth = 0;

    catacurses::window w_test_rule_border;
    catacurses::window w_test_rule_content;

    ui_adaptor ui;

    const auto init_windows = [&]( ui_adaptor & ui ) {
        const point iOffset( 15 + ( TERMX > FULL_SCREEN_WIDTH ? ( TERMX - FULL_SCREEN_WIDTH ) / 2 : 0 ),
                             5 + ( TERMY > FULL_SCREEN_HEIGHT ? ( TERMY - FULL_SCREEN_HEIGHT ) / 2 :
                                   0 ) );
        iContentHeight = FULL_SCREEN_HEIGHT - 8;
        iContentWidth = FULL_SCREEN_WIDTH - 30;

        w_test_rule_border = catacurses::newwin( iContentHeight + 2, iContentWidth,
                             iOffset );
        w_test_rule_content = catacurses::newwin( iContentHeight,
                              iContentWidth - 2,
                              iOffset + point::south_east );

        ui.position_from_window( w_test_rule_border );
    };
    init_windows( ui );
    ui.on_screen_resize( init_windows );

    int nmatch = vMatchingItems.size();
    const std::string buf = string_format( n_gettext( "%1$d item matches: %2$s",
                                           "%1$d items match: %2$s",
                                           nmatch ), nmatch, sRule );

    int iLine = 0;

    input_context ctxt( "AUTO_PICKUP_TEST" );
    ctxt.register_navigate_ui_list();
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );

    ui.on_redraw( [&]( const ui_adaptor & ) {
        draw_border( w_test_rule_border, BORDER_COLOR, buf, hilite( c_white ) );
        center_print( w_test_rule_border, iContentHeight + 1, red_background( c_white ),
                      _( "Won't display content or suffix matches" ) );
        wnoutrefresh( w_test_rule_border );

        // Clear the lines
        mvwrectf( w_test_rule_content, point::zero, c_black, ' ', 79, iContentHeight );

        calcStartPos( iStartPos, iLine, iContentHeight, vMatchingItems.size() );

        // display auto pickup
        for( int i = iStartPos; i < static_cast<int>( vMatchingItems.size() ); i++ ) {
            if( i >= iStartPos &&
                i < iStartPos + std::min<int>( iContentHeight, vMatchingItems.size() ) ) {
                nc_color cLineColor = c_white;

                mvwprintz( w_test_rule_content, point( 0, i - iStartPos ), cLineColor, "%d", i + 1 );
                mvwprintz( w_test_rule_content, point( 4, i - iStartPos ), cLineColor, "" );

                if( iLine == i ) {
                    wprintz( w_test_rule_content, c_yellow, ">> " );
                } else {
                    wprintz( w_test_rule_content, c_yellow, "   " );
                }

                wprintz( w_test_rule_content, iLine == i ? hilite( cLineColor ) : cLineColor, vMatchingItems[i] );
            }
        }

        wnoutrefresh( w_test_rule_content );
    } );

    while( true ) {
        ui_manager::redraw();

        const int recmax = static_cast<int>( vMatchingItems.size() );
        const int scroll_rate = recmax > 20 ? 10 : 3;
        const std::string action = ctxt.handle_input();
        if( navigate_ui_list( action, iLine, scroll_rate, recmax, true ) ) {
        } else if( action == "QUIT" ) {
            break;
        }
    }
}

bool player_settings::has_rule( const item *it )
{
    const std::string &name = it->tname( 1 );
    for( auto_pickup::rule &elem : character_rules ) {
        if( name.length() == elem.sRule.length() && ci_find_substr( name, elem.sRule ) != -1 ) {
            return true;
        }
    }
    return false;
}

void player_settings::add_rule( const item *it, bool include )
{
    character_rules.push_back( rule( it->tname( 1, false ), true, !include ) );
    create_rule( it );

    if( !get_option<bool>( "AUTO_PICKUP" ) &&
        query_yn( _( "Auto pickup is not enabled in the options.  Enable it now?" ) ) ) {
        get_options().get_option( "AUTO_PICKUP" ).setNext();
        get_options().save();
    }
}

void player_settings::remove_rule( const item *it )
{
    const std::string sRule = it->tname( 1, false );
    for( rule_list::iterator candidate = character_rules.begin();
         candidate != character_rules.end(); ++candidate ) {
        if( sRule.length() == candidate->sRule.length() &&
            ci_find_substr( sRule, candidate->sRule ) != -1 ) {
            character_rules.erase( candidate );
            invalidate();
            break;
        }
    }
}

bool player_settings::empty() const
{
    return global_rules.empty() && character_rules.empty();
}

bool check_special_rule( const std::map<material_id, int> &materials, std::string_view rule )
{
    char type = ' ';
    std::vector<std::string> filter;
    if( rule[1] == ':' ) {
        type = rule[0];
        filter = string_split( rule.substr( 2 ), ',' );
    }

    if( filter.empty() || materials.empty() ) {
        return false;
    }

    if( type == 'm' ) {
        return std::any_of( materials.begin(),
        materials.end(), [&filter]( const std::pair<material_id, int> &mat ) {
            return std::any_of( filter.begin(), filter.end(), [&mat]( std::string_view search ) {
                return lcmatch( mat.first->name(), search );
            } );
        } );

    } else if( type == 'M' ) {
        return std::all_of( materials.begin(),
        materials.end(), [&filter]( const std::pair<material_id, int> &mat ) {
            return std::any_of( filter.begin(), filter.end(), [&mat]( std::string_view search ) {
                return lcmatch( mat.first->name(), search );
            } );
        } );
    }

    return false;
}

//Special case. Required for NPC harvest auto pickup. Ignores material rules.
void npc_settings::create_rule( const std::string &to_match )
{
    rules.create_rule( map_items, to_match );
}

void rule_list::create_rule( cache &map_items, const std::string &to_match )
{
    for( const rule &elem : *this ) {
        if( !elem.bActive || !wildcard_match( to_match, elem.sRule ) ) {
            continue;
        }

        map_items[ to_match ] = elem.bExclude ? rule_state::BLACKLISTED : rule_state::WHITELISTED;
    }
}

void player_settings::create_rule( const item *it )
{
    // TODO: change it to be a reference
    global_rules.create_rule( map_items, *it );
    character_rules.create_rule( map_items, *it );
}

void rule_list::create_rule( cache &map_items, const item &it )
{
    const std::string to_match = it.tname( 1, false );

    for( const rule &elem : *this ) {
        if( !elem.bActive ) {
            continue;
        }
        if( !check_special_rule( it.made_of(), elem.sRule ) &&
            !wildcard_match( to_match, elem.sRule ) ) {
            continue;
        }

        map_items[ to_match ] = elem.bExclude ? rule_state::BLACKLISTED : rule_state::WHITELISTED;
    }
}

void player_settings::refresh_map_items( cache &map_items ) const
{
    //process include/exclude in order of rules, global first, then character specific
    //if a specific item is being added, all the rules need to be checked now
    //may have some performance issues since exclusion needs to check all items also
    global_rules.refresh_map_items( map_items );
    character_rules.refresh_map_items( map_items );
}

void rule_list::refresh_map_items( cache &map_items ) const
{
    for( const rule &elem : *this ) {
        if( elem.sRule.empty() || !elem.bActive ) {
            continue;
        }

        if( !elem.bExclude ) {
            //Check include patterns against all itemfactory items
            for( const itype *e : item_controller->all() ) {
                const std::string &cur_item = e->nname( 1 );

                if( !check_special_rule( e->materials, elem.sRule ) && !wildcard_match( cur_item, elem.sRule ) ) {
                    continue;
                }

                map_items[ cur_item ] = rule_state::WHITELISTED;
                map_items.temp_items[ cur_item ] = e;
            }
        } else {
            //only re-exclude items from the existing mapping for now
            //new exclusions will process during pickup attempts
            for( auto &map_item : map_items ) {
                if( !check_special_rule( map_items.temp_items[ map_item.first ]->materials, elem.sRule ) &&
                    !wildcard_match( map_item.first, elem.sRule ) ) {
                    continue;
                }

                map_items[ map_item.first ] = rule_state::BLACKLISTED;
            }
        }
    }
}

rule_state base_settings::check_item( const std::string &sItemName ) const
{
    if( !map_items.ready ) {
        recreate();
    }

    const auto iter = map_items.find( sItemName );
    if( iter != map_items.end() ) {
        return iter->second;
    }

    return rule_state::NONE;
}

void player_settings::clear_character_rules()
{
    character_rules.clear();
    invalidate();
}

bool player_settings::save_character()
{
    return save( true );
}

bool player_settings::save_global()
{
    return save( false );
}

bool player_settings::save( const bool bCharacter )
{
    cata_path savefile = PATH_INFO::autopickup();

    if( bCharacter ) {
        savefile = PATH_INFO::player_base_save_path() + ".apu.json";

        const cata_path player_save = PATH_INFO::player_base_save_path() + ".sav";
        const cata_path player_save_zzip = player_save + ".zzip";
        //Character not saved yet.
        if( !file_exist( player_save ) || !file_exist( player_save_zzip ) ) {
            return true;
        }
    }

    return write_to_file( savefile, [&]( std::ostream & fout ) {
        JsonOut jout( fout, true );
        ( bCharacter ? character_rules : global_rules ).serialize( jout );
    }, _( "auto pickup configuration" ) );
}

void player_settings::load_character()
{
    load( true );
}

void player_settings::load_global()
{
    load( false );
}

void player_settings::load( const bool bCharacter )
{
    cata_path sFile = PATH_INFO::autopickup();
    if( bCharacter ) {
        sFile = PATH_INFO::player_base_save_path() + ".apu.json";
    }

    read_from_file_optional_json( sFile, [&]( const JsonValue & jv ) {
        ( bCharacter ? character_rules : global_rules ).deserialize( jv );
    } ) ;

    invalidate();
}

void rule::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "rule", sRule );
    jsout.member( "active", bActive );
    jsout.member( "exclude", bExclude );
    jsout.end_object();
}

void rule_list::serialize( JsonOut &jsout ) const
{
    jsout.start_array();
    for( const rule &elem : *this ) {
        elem.serialize( jsout );
    }
    jsout.end_array();
}

void rule::deserialize( const JsonObject &jo )
{
    sRule = jo.get_string( "rule" );
    bActive = jo.get_bool( "active" );
    bExclude = jo.get_bool( "exclude" );
}

void rule_list::deserialize( const JsonArray &ja )
{
    clear();

    for( JsonObject jo : ja ) {
        rule tmp;
        tmp.deserialize( jo );
        push_back( tmp );
    }
}

void npc_settings::show( const std::string &name )
{
    user_interface ui;
    ui.title = string_format( _( "Pickup rules for %s" ), name );
    ui.tabs.emplace_back( name, rules );
    ui.show();
    // Don't need to save the rules here, it will be save along with the NPC object itself.
    if( !ui.bStuffChanged ) {
        return;
    }
    invalidate();
}

void npc_settings::serialize( JsonOut &jsout ) const
{
    rules.serialize( jsout );
}

void npc_settings::deserialize( const JsonArray &ja )
{
    rules.deserialize( ja );
}

void npc_settings::refresh_map_items( cache &map_items ) const
{
    rules.refresh_map_items( map_items );
}

bool npc_settings::empty() const
{
    return rules.empty();
}

void base_settings::recreate() const
{
    map_items.clear();
    map_items.temp_items.clear();
    refresh_map_items( map_items );
    map_items.ready = true;
    map_items.temp_items.clear();
}

void base_settings::invalidate()
{
    map_items.ready = false;
}
