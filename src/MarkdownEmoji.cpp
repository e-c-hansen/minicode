// MarkdownEmoji.cpp — see MarkdownEmoji.h.
#include "MarkdownEmoji.h"

#include <cstdint>
#include <unordered_map>

namespace {

struct Entry {
    const char* name;
    uint32_t cp;
};

// GitHub's names (gemoji) for the emoji people write most: faces, hands,
// hearts, animals, food, travel, objects and the symbols READMEs and
// changelogs use. One code point each; the variation selector is added
// where needed (presentationSelector below).
const Entry kTable[] = {
    // Faces
    {"grinning", 0x1F600}, {"smiley", 0x1F603}, {"smile", 0x1F604}, {"grin", 0x1F601},
    {"laughing", 0x1F606}, {"satisfied", 0x1F606}, {"sweat_smile", 0x1F605}, {"rofl", 0x1F923},
    {"joy", 0x1F602}, {"slightly_smiling_face", 0x1F642}, {"upside_down_face", 0x1F643},
    {"wink", 0x1F609}, {"blush", 0x1F60A}, {"innocent", 0x1F607},
    {"smiling_face_with_three_hearts", 0x1F970}, {"heart_eyes", 0x1F60D}, {"star_struck", 0x1F929},
    {"kissing_heart", 0x1F618}, {"kissing", 0x1F617}, {"relaxed", 0x263A},
    {"kissing_closed_eyes", 0x1F61A}, {"kissing_smiling_eyes", 0x1F619}, {"yum", 0x1F60B},
    {"stuck_out_tongue", 0x1F61B}, {"stuck_out_tongue_winking_eye", 0x1F61C}, {"zany_face", 0x1F92A},
    {"stuck_out_tongue_closed_eyes", 0x1F61D}, {"money_mouth_face", 0x1F911}, {"hugs", 0x1F917},
    {"hand_over_mouth", 0x1F92D}, {"shushing_face", 0x1F92B}, {"thinking", 0x1F914},
    {"zipper_mouth_face", 0x1F910}, {"raised_eyebrow", 0x1F928}, {"neutral_face", 0x1F610},
    {"expressionless", 0x1F611}, {"no_mouth", 0x1F636}, {"smirk", 0x1F60F}, {"unamused", 0x1F612},
    {"roll_eyes", 0x1F644}, {"grimacing", 0x1F62C}, {"lying_face", 0x1F925}, {"relieved", 0x1F60C},
    {"pensive", 0x1F614}, {"sleepy", 0x1F62A}, {"drooling_face", 0x1F924}, {"sleeping", 0x1F634},
    {"mask", 0x1F637}, {"face_with_thermometer", 0x1F912}, {"face_with_head_bandage", 0x1F915},
    {"nauseated_face", 0x1F922}, {"vomiting_face", 0x1F92E}, {"sneezing_face", 0x1F927},
    {"hot_face", 0x1F975}, {"cold_face", 0x1F976}, {"woozy_face", 0x1F974}, {"dizzy_face", 0x1F635},
    {"exploding_head", 0x1F92F}, {"cowboy_hat_face", 0x1F920}, {"partying_face", 0x1F973},
    {"sunglasses", 0x1F60E}, {"nerd_face", 0x1F913}, {"monocle_face", 0x1F9D0},
    {"confused", 0x1F615}, {"worried", 0x1F61F}, {"slightly_frowning_face", 0x1F641},
    {"frowning_face", 0x2639}, {"open_mouth", 0x1F62E}, {"hushed", 0x1F62F},
    {"astonished", 0x1F632}, {"flushed", 0x1F633}, {"pleading_face", 0x1F97A},
    {"frowning", 0x1F626}, {"anguished", 0x1F627}, {"fearful", 0x1F628}, {"cold_sweat", 0x1F630},
    {"disappointed_relieved", 0x1F625}, {"cry", 0x1F622}, {"sob", 0x1F62D}, {"scream", 0x1F631},
    {"confounded", 0x1F616}, {"persevere", 0x1F623}, {"disappointed", 0x1F61E}, {"sweat", 0x1F613},
    {"weary", 0x1F629}, {"tired_face", 0x1F62B}, {"yawning_face", 0x1F971}, {"triumph", 0x1F624},
    {"rage", 0x1F621}, {"pout", 0x1F621}, {"angry", 0x1F620}, {"cursing_face", 0x1F92C},
    {"smiling_imp", 0x1F608}, {"imp", 0x1F47F}, {"skull", 0x1F480},
    {"skull_and_crossbones", 0x2620}, {"hankey", 0x1F4A9}, {"poop", 0x1F4A9}, {"shit", 0x1F4A9},
    {"clown_face", 0x1F921}, {"japanese_ogre", 0x1F479}, {"ghost", 0x1F47B}, {"alien", 0x1F47D},
    {"space_invader", 0x1F47E}, {"robot", 0x1F916}, {"smiley_cat", 0x1F63A}, {"smile_cat", 0x1F638},
    {"joy_cat", 0x1F639}, {"heart_eyes_cat", 0x1F63B}, {"scream_cat", 0x1F640},
    {"crying_cat_face", 0x1F63F}, {"see_no_evil", 0x1F648}, {"hear_no_evil", 0x1F649},
    {"speak_no_evil", 0x1F64A},
    // Hearts and marks
    {"kiss", 0x1F48B}, {"love_letter", 0x1F48C}, {"cupid", 0x1F498}, {"gift_heart", 0x1F49D},
    {"sparkling_heart", 0x1F496}, {"heartpulse", 0x1F497}, {"heartbeat", 0x1F493},
    {"revolving_hearts", 0x1F49E}, {"two_hearts", 0x1F495}, {"heart_decoration", 0x1F49F},
    {"heavy_heart_exclamation", 0x2763}, {"broken_heart", 0x1F494}, {"heart", 0x2764},
    {"orange_heart", 0x1F9E1}, {"yellow_heart", 0x1F49B}, {"green_heart", 0x1F49A},
    {"blue_heart", 0x1F499}, {"purple_heart", 0x1F49C}, {"brown_heart", 0x1F90E},
    {"black_heart", 0x1F5A4}, {"white_heart", 0x1F90D}, {"100", 0x1F4AF}, {"anger", 0x1F4A2},
    {"boom", 0x1F4A5}, {"collision", 0x1F4A5}, {"dizzy", 0x1F4AB}, {"sweat_drops", 0x1F4A6},
    {"dash", 0x1F4A8}, {"hole", 0x1F573}, {"bomb", 0x1F4A3}, {"speech_balloon", 0x1F4AC},
    {"thought_balloon", 0x1F4AD}, {"zzz", 0x1F4A4},
    // Hands and body
    {"wave", 0x1F44B}, {"raised_back_of_hand", 0x1F91A}, {"raised_hand", 0x270B}, {"hand", 0x270B},
    {"vulcan_salute", 0x1F596}, {"ok_hand", 0x1F44C}, {"pinching_hand", 0x1F90F}, {"v", 0x270C},
    {"crossed_fingers", 0x1F91E}, {"love_you_gesture", 0x1F91F}, {"metal", 0x1F918},
    {"call_me_hand", 0x1F919}, {"point_left", 0x1F448}, {"point_right", 0x1F449},
    {"point_up_2", 0x1F446}, {"middle_finger", 0x1F595}, {"point_down", 0x1F447},
    {"point_up", 0x261D}, {"+1", 0x1F44D}, {"thumbsup", 0x1F44D}, {"-1", 0x1F44E},
    {"thumbsdown", 0x1F44E}, {"fist_raised", 0x270A}, {"fist", 0x270A}, {"fist_oncoming", 0x1F44A},
    {"facepunch", 0x1F44A}, {"punch", 0x1F44A}, {"fist_left", 0x1F91B}, {"fist_right", 0x1F91C},
    {"clap", 0x1F44F}, {"raised_hands", 0x1F64C}, {"open_hands", 0x1F450},
    {"palms_up_together", 0x1F932}, {"handshake", 0x1F91D}, {"pray", 0x1F64F},
    {"writing_hand", 0x270D}, {"nail_care", 0x1F485}, {"selfie", 0x1F933}, {"muscle", 0x1F4AA},
    {"mechanical_arm", 0x1F9BE}, {"leg", 0x1F9B5}, {"foot", 0x1F9B6}, {"ear", 0x1F442},
    {"nose", 0x1F443}, {"brain", 0x1F9E0}, {"eyes", 0x1F440}, {"eye", 0x1F441},
    {"tongue", 0x1F445}, {"lips", 0x1F444},
    // People
    {"baby", 0x1F476}, {"child", 0x1F9D2}, {"boy", 0x1F466}, {"girl", 0x1F467}, {"adult", 0x1F9D1},
    {"man", 0x1F468}, {"woman", 0x1F469}, {"older_man", 0x1F474}, {"older_woman", 0x1F475},
    {"construction_worker", 0x1F477}, {"guardsman", 0x1F482}, {"detective", 0x1F575},
    {"ninja", 0x1F977}, {"superhero", 0x1F9B8}, {"mage", 0x1F9D9}, {"zombie", 0x1F9DF},
    {"bow", 0x1F647}, {"raising_hand", 0x1F64B}, {"no_good", 0x1F645}, {"ok_woman", 0x1F646},
    {"information_desk_person", 0x1F481}, {"tipping_hand_person", 0x1F481}, {"shrug", 0x1F937},
    {"facepalm", 0x1F926}, {"runner", 0x1F3C3}, {"running", 0x1F3C3}, {"walking", 0x1F6B6},
    {"dancer", 0x1F483}, {"family", 0x1F46A}, {"couple", 0x1F46B},
    {"busts_in_silhouette", 0x1F465}, {"bust_in_silhouette", 0x1F464}, {"footprints", 0x1F463},
    // Animals and plants
    {"dog", 0x1F436}, {"cat", 0x1F431}, {"mouse", 0x1F42D}, {"hamster", 0x1F439},
    {"rabbit", 0x1F430}, {"fox_face", 0x1F98A}, {"bear", 0x1F43B}, {"panda_face", 0x1F43C},
    {"koala", 0x1F428}, {"tiger", 0x1F42F}, {"lion", 0x1F981}, {"cow", 0x1F42E}, {"pig", 0x1F437},
    {"frog", 0x1F438}, {"monkey_face", 0x1F435}, {"monkey", 0x1F412}, {"chicken", 0x1F414},
    {"penguin", 0x1F427}, {"bird", 0x1F426}, {"baby_chick", 0x1F424}, {"eagle", 0x1F985},
    {"duck", 0x1F986}, {"owl", 0x1F989}, {"bat", 0x1F987}, {"wolf", 0x1F43A}, {"boar", 0x1F417},
    {"horse", 0x1F434}, {"unicorn", 0x1F984}, {"bee", 0x1F41D}, {"honeybee", 0x1F41D},
    {"bug", 0x1F41B}, {"butterfly", 0x1F98B}, {"snail", 0x1F40C}, {"beetle", 0x1F41E},
    {"lady_beetle", 0x1F41E}, {"ant", 0x1F41C}, {"spider", 0x1F577}, {"scorpion", 0x1F982},
    {"turtle", 0x1F422}, {"snake", 0x1F40D}, {"lizard", 0x1F98E}, {"t-rex", 0x1F996},
    {"sauropod", 0x1F995}, {"octopus", 0x1F419}, {"squid", 0x1F991}, {"crab", 0x1F980},
    {"lobster", 0x1F99E}, {"shrimp", 0x1F990}, {"tropical_fish", 0x1F420}, {"fish", 0x1F41F},
    {"blowfish", 0x1F421}, {"dolphin", 0x1F42C}, {"whale", 0x1F433}, {"whale2", 0x1F40B},
    {"shark", 0x1F988}, {"crocodile", 0x1F40A}, {"elephant", 0x1F418}, {"camel", 0x1F42B},
    {"giraffe", 0x1F992}, {"dog2", 0x1F415}, {"cat2", 0x1F408}, {"rooster", 0x1F413},
    {"turkey", 0x1F983}, {"dove", 0x1F54A}, {"rat", 0x1F400}, {"mouse2", 0x1F401},
    {"chipmunk", 0x1F43F}, {"hedgehog", 0x1F994}, {"dragon", 0x1F409}, {"dragon_face", 0x1F432},
    {"paw_prints", 0x1F43E}, {"feet", 0x1F43E}, {"sloth", 0x1F9A5}, {"otter", 0x1F9A6},
    {"parrot", 0x1F99C}, {"flamingo", 0x1F9A9}, {"peacock", 0x1F99A}, {"swan", 0x1F9A2},
    {"llama", 0x1F999}, {"kangaroo", 0x1F998}, {"badger", 0x1F9A1}, {"raccoon", 0x1F99D},
    {"bouquet", 0x1F490}, {"cherry_blossom", 0x1F338}, {"rose", 0x1F339},
    {"wilted_flower", 0x1F940}, {"hibiscus", 0x1F33A}, {"sunflower", 0x1F33B},
    {"blossom", 0x1F33C}, {"tulip", 0x1F337}, {"seedling", 0x1F331}, {"evergreen_tree", 0x1F332},
    {"deciduous_tree", 0x1F333}, {"palm_tree", 0x1F334}, {"cactus", 0x1F335}, {"herb", 0x1F33F},
    {"shamrock", 0x2618}, {"four_leaf_clover", 0x1F340}, {"maple_leaf", 0x1F341},
    {"fallen_leaf", 0x1F342}, {"leaves", 0x1F343}, {"mushroom", 0x1F344},
    {"ear_of_rice", 0x1F33E},
    // Food and drink
    {"grapes", 0x1F347}, {"melon", 0x1F348}, {"watermelon", 0x1F349}, {"tangerine", 0x1F34A},
    {"orange", 0x1F34A}, {"lemon", 0x1F34B}, {"banana", 0x1F34C}, {"pineapple", 0x1F34D},
    {"mango", 0x1F96D}, {"apple", 0x1F34E}, {"green_apple", 0x1F34F}, {"pear", 0x1F350},
    {"peach", 0x1F351}, {"cherries", 0x1F352}, {"strawberry", 0x1F353}, {"kiwi_fruit", 0x1F95D},
    {"tomato", 0x1F345}, {"coconut", 0x1F965}, {"avocado", 0x1F951}, {"eggplant", 0x1F346},
    {"potato", 0x1F954}, {"carrot", 0x1F955}, {"corn", 0x1F33D}, {"hot_pepper", 0x1F336},
    {"cucumber", 0x1F952}, {"broccoli", 0x1F966}, {"garlic", 0x1F9C4}, {"onion", 0x1F9C5},
    {"peanuts", 0x1F95C}, {"bread", 0x1F35E}, {"croissant", 0x1F950},
    {"baguette_bread", 0x1F956}, {"pretzel", 0x1F968}, {"bagel", 0x1F96F}, {"pancakes", 0x1F95E},
    {"cheese", 0x1F9C0}, {"meat_on_bone", 0x1F356}, {"poultry_leg", 0x1F357}, {"bacon", 0x1F953},
    {"hamburger", 0x1F354}, {"fries", 0x1F35F}, {"pizza", 0x1F355}, {"hotdog", 0x1F32D},
    {"sandwich", 0x1F96A}, {"taco", 0x1F32E}, {"burrito", 0x1F32F}, {"egg", 0x1F95A},
    {"fried_egg", 0x1F373}, {"salad", 0x1F957}, {"popcorn", 0x1F37F}, {"salt", 0x1F9C2},
    {"bento", 0x1F371}, {"rice", 0x1F35A}, {"curry", 0x1F35B}, {"ramen", 0x1F35C},
    {"spaghetti", 0x1F35D}, {"sushi", 0x1F363}, {"fried_shrimp", 0x1F364}, {"dumpling", 0x1F95F},
    {"icecream", 0x1F366}, {"shaved_ice", 0x1F367}, {"ice_cream", 0x1F368}, {"doughnut", 0x1F369},
    {"cookie", 0x1F36A}, {"birthday", 0x1F382}, {"cake", 0x1F370}, {"cupcake", 0x1F9C1},
    {"pie", 0x1F967}, {"chocolate_bar", 0x1F36B}, {"candy", 0x1F36C}, {"lollipop", 0x1F36D},
    {"honey_pot", 0x1F36F}, {"coffee", 0x2615}, {"tea", 0x1F375}, {"baby_bottle", 0x1F37C},
    {"milk_glass", 0x1F95B}, {"sake", 0x1F376}, {"champagne", 0x1F37E}, {"wine_glass", 0x1F377},
    {"cocktail", 0x1F378}, {"tropical_drink", 0x1F379}, {"beer", 0x1F37A}, {"beers", 0x1F37B},
    {"clinking_glasses", 0x1F942}, {"tumbler_glass", 0x1F943}, {"cup_with_straw", 0x1F964},
    {"fork_and_knife", 0x1F374}, {"spoon", 0x1F944}, {"hocho", 0x1F52A}, {"knife", 0x1F52A},
    // Places, travel and weather
    {"earth_africa", 0x1F30D}, {"earth_americas", 0x1F30E}, {"earth_asia", 0x1F30F},
    {"globe_with_meridians", 0x1F310}, {"world_map", 0x1F5FA}, {"japan", 0x1F5FE},
    {"mountain", 0x26F0}, {"volcano", 0x1F30B}, {"mount_fuji", 0x1F5FB}, {"camping", 0x1F3D5},
    {"beach_umbrella", 0x1F3D6}, {"desert", 0x1F3DC}, {"desert_island", 0x1F3DD},
    {"house", 0x1F3E0}, {"house_with_garden", 0x1F3E1}, {"office", 0x1F3E2},
    {"hospital", 0x1F3E5}, {"bank", 0x1F3E6}, {"hotel", 0x1F3E8}, {"school", 0x1F3EB},
    {"factory", 0x1F3ED}, {"european_castle", 0x1F3F0}, {"tokyo_tower", 0x1F5FC},
    {"statue_of_liberty", 0x1F5FD}, {"church", 0x26EA}, {"tent", 0x26FA}, {"foggy", 0x1F301},
    {"night_with_stars", 0x1F303}, {"sunrise", 0x1F305}, {"city_sunset", 0x1F306},
    {"bridge_at_night", 0x1F309}, {"carousel_horse", 0x1F3A0}, {"ferris_wheel", 0x1F3A1},
    {"roller_coaster", 0x1F3A2}, {"train", 0x1F68B}, {"steam_locomotive", 0x1F682},
    {"bullettrain_side", 0x1F684}, {"metro", 0x1F687}, {"bus", 0x1F68C}, {"ambulance", 0x1F691},
    {"fire_engine", 0x1F692}, {"police_car", 0x1F693}, {"taxi", 0x1F695}, {"car", 0x1F697},
    {"red_car", 0x1F697}, {"blue_car", 0x1F699}, {"truck", 0x1F69A}, {"tractor", 0x1F69C},
    {"racing_car", 0x1F3CE}, {"motorcycle", 0x1F3CD}, {"bike", 0x1F6B2},
    {"kick_scooter", 0x1F6F4}, {"busstop", 0x1F68F}, {"fuelpump", 0x26FD},
    {"rotating_light", 0x1F6A8}, {"traffic_light", 0x1F6A5}, {"vertical_traffic_light", 0x1F6A6},
    {"construction", 0x1F6A7}, {"anchor", 0x2693}, {"boat", 0x26F5}, {"sailboat", 0x26F5},
    {"canoe", 0x1F6F6}, {"speedboat", 0x1F6A4}, {"ship", 0x1F6A2}, {"airplane", 0x2708},
    {"small_airplane", 0x1F6E9}, {"flight_departure", 0x1F6EB}, {"flight_arrival", 0x1F6EC},
    {"parachute", 0x1FA82}, {"seat", 0x1F4BA}, {"helicopter", 0x1F681}, {"rocket", 0x1F680},
    {"flying_saucer", 0x1F6F8}, {"artificial_satellite", 0x1F6F0}, {"hourglass", 0x231B},
    {"hourglass_flowing_sand", 0x23F3}, {"watch", 0x231A}, {"alarm_clock", 0x23F0},
    {"stopwatch", 0x23F1}, {"timer_clock", 0x23F2}, {"new_moon", 0x1F311}, {"moon", 0x1F314},
    {"full_moon", 0x1F315}, {"crescent_moon", 0x1F319}, {"thermometer", 0x1F321},
    {"sunny", 0x2600}, {"star", 0x2B50}, {"star2", 0x1F31F}, {"stars", 0x1F320},
    {"milky_way", 0x1F30C}, {"cloud", 0x2601}, {"partly_sunny", 0x26C5},
    {"cloud_with_rain", 0x1F327}, {"cloud_with_lightning_and_rain", 0x26C8},
    {"tornado", 0x1F32A}, {"fog", 0x1F32B}, {"rainbow", 0x1F308}, {"umbrella", 0x2614},
    {"zap", 0x26A1}, {"snowflake", 0x2744}, {"snowman", 0x26C4}, {"fire", 0x1F525},
    {"droplet", 0x1F4A7}, {"ocean", 0x1F30A}, {"sun_with_face", 0x1F31E}, {"cyclone", 0x1F300},
    {"comet", 0x2604},
    // Activities, music and pictures
    {"jack_o_lantern", 0x1F383}, {"christmas_tree", 0x1F384}, {"fireworks", 0x1F386},
    {"sparkler", 0x1F387}, {"sparkles", 0x2728}, {"balloon", 0x1F388}, {"tada", 0x1F389},
    {"confetti_ball", 0x1F38A}, {"ribbon", 0x1F380}, {"gift", 0x1F381}, {"trophy", 0x1F3C6},
    {"medal_sports", 0x1F3C5}, {"1st_place_medal", 0x1F947}, {"2nd_place_medal", 0x1F948},
    {"3rd_place_medal", 0x1F949}, {"soccer", 0x26BD}, {"baseball", 0x26BE},
    {"basketball", 0x1F3C0}, {"volleyball", 0x1F3D0}, {"football", 0x1F3C8}, {"tennis", 0x1F3BE},
    {"bowling", 0x1F3B3}, {"golf", 0x26F3}, {"goal_net", 0x1F945}, {"ice_skate", 0x26F8},
    {"fishing_pole_and_fish", 0x1F3A3}, {"ski", 0x1F3BF}, {"dart", 0x1F3AF}, {"8ball", 0x1F3B1},
    {"crystal_ball", 0x1F52E}, {"video_game", 0x1F3AE}, {"joystick", 0x1F579},
    {"slot_machine", 0x1F3B0}, {"game_die", 0x1F3B2}, {"jigsaw", 0x1F9E9}, {"teddy_bear", 0x1F9F8},
    {"spades", 0x2660}, {"hearts", 0x2665}, {"diamonds", 0x2666}, {"clubs", 0x2663},
    {"chess_pawn", 0x265F}, {"black_joker", 0x1F0CF}, {"performing_arts", 0x1F3AD},
    {"art", 0x1F3A8}, {"thread", 0x1F9F5}, {"yarn", 0x1F9F6}, {"musical_note", 0x1F3B5},
    {"notes", 0x1F3B6}, {"microphone", 0x1F3A4}, {"headphones", 0x1F3A7}, {"radio", 0x1F4FB},
    {"saxophone", 0x1F3B7}, {"guitar", 0x1F3B8}, {"musical_keyboard", 0x1F3B9},
    {"trumpet", 0x1F3BA}, {"violin", 0x1F3BB}, {"drum", 0x1F941}, {"studio_microphone", 0x1F399},
    {"movie_camera", 0x1F3A5}, {"clapper", 0x1F3AC}, {"tv", 0x1F4FA}, {"camera", 0x1F4F7},
    {"camera_flash", 0x1F4F8}, {"video_camera", 0x1F4F9}, {"film_strip", 0x1F39E},
    {"ticket", 0x1F3AB}, {"tickets", 0x1F39F}, {"running_shirt_with_sash", 0x1F3BD},
    {"checkered_flag", 0x1F3C1}, {"triangular_flag_on_post", 0x1F6A9}, {"white_flag", 0x1F3F3},
    {"black_flag", 0x1F3F4},
    // Objects
    {"iphone", 0x1F4F1}, {"calling", 0x1F4F2}, {"phone", 0x260E}, {"telephone", 0x260E},
    {"telephone_receiver", 0x1F4DE}, {"pager", 0x1F4DF}, {"fax", 0x1F4E0}, {"battery", 0x1F50B},
    {"electric_plug", 0x1F50C}, {"computer", 0x1F4BB}, {"desktop_computer", 0x1F5A5},
    {"printer", 0x1F5A8}, {"keyboard", 0x2328}, {"computer_mouse", 0x1F5B1},
    {"trackball", 0x1F5B2}, {"minidisc", 0x1F4BD}, {"floppy_disk", 0x1F4BE}, {"cd", 0x1F4BF},
    {"dvd", 0x1F4C0}, {"abacus", 0x1F9EE}, {"mag", 0x1F50D}, {"mag_right", 0x1F50E},
    {"bulb", 0x1F4A1}, {"flashlight", 0x1F526}, {"candle", 0x1F56F}, {"book", 0x1F4D6},
    {"open_book", 0x1F4D6}, {"closed_book", 0x1F4D5}, {"green_book", 0x1F4D7},
    {"blue_book", 0x1F4D8}, {"orange_book", 0x1F4D9}, {"books", 0x1F4DA}, {"notebook", 0x1F4D3},
    {"ledger", 0x1F4D2}, {"page_with_curl", 0x1F4C3}, {"scroll", 0x1F4DC},
    {"page_facing_up", 0x1F4C4}, {"newspaper", 0x1F4F0}, {"bookmark_tabs", 0x1F4D1},
    {"bookmark", 0x1F516}, {"label", 0x1F3F7}, {"moneybag", 0x1F4B0}, {"yen", 0x1F4B4},
    {"dollar", 0x1F4B5}, {"euro", 0x1F4B6}, {"pound", 0x1F4B7}, {"money_with_wings", 0x1F4B8},
    {"credit_card", 0x1F4B3}, {"receipt", 0x1F9FE}, {"chart", 0x1F4B9}, {"email", 0x1F4E7},
    {"e-mail", 0x1F4E7}, {"envelope", 0x2709}, {"incoming_envelope", 0x1F4E8},
    {"envelope_with_arrow", 0x1F4E9}, {"outbox_tray", 0x1F4E4}, {"inbox_tray", 0x1F4E5},
    {"package", 0x1F4E6}, {"mailbox", 0x1F4EB}, {"postbox", 0x1F4EE}, {"ballot_box", 0x1F5F3},
    {"pencil2", 0x270F}, {"black_nib", 0x2712}, {"fountain_pen", 0x1F58B}, {"pen", 0x1F58A},
    {"paintbrush", 0x1F58C}, {"crayon", 0x1F58D}, {"memo", 0x1F4DD}, {"pencil", 0x1F4DD},
    {"briefcase", 0x1F4BC}, {"file_folder", 0x1F4C1}, {"open_file_folder", 0x1F4C2},
    {"card_index_dividers", 0x1F5C2}, {"date", 0x1F4C5}, {"calendar", 0x1F4C6},
    {"spiral_notepad", 0x1F5D2}, {"spiral_calendar", 0x1F5D3}, {"card_index", 0x1F4C7},
    {"chart_with_upwards_trend", 0x1F4C8}, {"chart_with_downwards_trend", 0x1F4C9},
    {"bar_chart", 0x1F4CA}, {"clipboard", 0x1F4CB}, {"pushpin", 0x1F4CC},
    {"round_pushpin", 0x1F4CD}, {"paperclip", 0x1F4CE}, {"paperclips", 0x1F587},
    {"straight_ruler", 0x1F4CF}, {"triangular_ruler", 0x1F4D0}, {"scissors", 0x2702},
    {"card_file_box", 0x1F5C3}, {"file_cabinet", 0x1F5C4}, {"wastebasket", 0x1F5D1},
    {"lock", 0x1F512}, {"unlock", 0x1F513}, {"lock_with_ink_pen", 0x1F50F},
    {"closed_lock_with_key", 0x1F510}, {"key", 0x1F511}, {"old_key", 0x1F5DD},
    {"hammer", 0x1F528}, {"axe", 0x1FA93}, {"pick", 0x26CF}, {"hammer_and_wrench", 0x1F6E0},
    {"dagger", 0x1F5E1}, {"crossed_swords", 0x2694}, {"gun", 0x1F52B}, {"shield", 0x1F6E1},
    {"wrench", 0x1F527}, {"nut_and_bolt", 0x1F529}, {"gear", 0x2699}, {"clamp", 0x1F5DC},
    {"balance_scale", 0x2696}, {"link", 0x1F517}, {"chains", 0x26D3}, {"toolbox", 0x1F9F0},
    {"magnet", 0x1F9F2}, {"bricks", 0x1F9F1}, {"alembic", 0x2697}, {"test_tube", 0x1F9EA},
    {"petri_dish", 0x1F9EB}, {"dna", 0x1F9EC}, {"microscope", 0x1F52C}, {"telescope", 0x1F52D},
    {"satellite", 0x1F4E1}, {"syringe", 0x1F489}, {"pill", 0x1F48A},
    {"adhesive_bandage", 0x1FA79}, {"stethoscope", 0x1FA7A}, {"door", 0x1F6AA}, {"bed", 0x1F6CF},
    {"couch_and_lamp", 0x1F6CB}, {"chair", 0x1FA91}, {"toilet", 0x1F6BD}, {"shower", 0x1F6BF},
    {"bathtub", 0x1F6C1}, {"broom", 0x1F9F9}, {"basket", 0x1F9FA}, {"roll_of_paper", 0x1F9FB},
    {"soap", 0x1F9FC}, {"sponge", 0x1F9FD}, {"fire_extinguisher", 0x1F9EF},
    {"shopping_cart", 0x1F6D2}, {"smoking", 0x1F6AC}, {"coffin", 0x26B0}, {"moyai", 0x1F5FF},
    {"bell", 0x1F514}, {"no_bell", 0x1F515}, {"loudspeaker", 0x1F4E2}, {"mega", 0x1F4E3},
    {"mute", 0x1F507}, {"speaker", 0x1F508}, {"sound", 0x1F509}, {"loud_sound", 0x1F50A},
    {"gem", 0x1F48E}, {"ring", 0x1F48D}, {"crown", 0x1F451}, {"tophat", 0x1F3A9},
    {"mortar_board", 0x1F393}, {"eyeglasses", 0x1F453}, {"dark_sunglasses", 0x1F576},
    {"necktie", 0x1F454}, {"shirt", 0x1F455}, {"tshirt", 0x1F455}, {"jeans", 0x1F456},
    {"dress", 0x1F457}, {"lipstick", 0x1F484}, {"handbag", 0x1F45C}, {"school_satchel", 0x1F392},
    {"athletic_shoe", 0x1F45F}, {"high_heel", 0x1F460}, {"lab_coat", 0x1F97C},
    {"goggles", 0x1F97D}, {"safety_vest", 0x1F9BA}, {"passport_control", 0x1F6C2},
    {"children_crossing", 0x1F6B8},
    // Symbols
    {"white_check_mark", 0x2705}, {"heavy_check_mark", 0x2714}, {"ballot_box_with_check", 0x2611},
    {"x", 0x274C}, {"negative_squared_cross_mark", 0x274E}, {"heavy_multiplication_x", 0x2716},
    {"heavy_plus_sign", 0x2795}, {"heavy_minus_sign", 0x2796}, {"heavy_division_sign", 0x2797},
    {"curly_loop", 0x27B0}, {"loop", 0x27BF}, {"question", 0x2753}, {"grey_question", 0x2754},
    {"exclamation", 0x2757}, {"heavy_exclamation_mark", 0x2757}, {"grey_exclamation", 0x2755},
    {"bangbang", 0x203C}, {"interrobang", 0x2049}, {"warning", 0x26A0}, {"no_entry", 0x26D4},
    {"no_entry_sign", 0x1F6AB}, {"stop_sign", 0x1F6D1}, {"recycle", 0x267B},
    {"beginner", 0x1F530}, {"trident", 0x1F531}, {"o", 0x2B55}, {"information_source", 0x2139},
    {"abc", 0x1F524}, {"abcd", 0x1F521}, {"capital_abcd", 0x1F520}, {"1234", 0x1F522},
    {"symbols", 0x1F523}, {"ok", 0x1F197}, {"new", 0x1F195}, {"free", 0x1F193}, {"up", 0x1F199},
    {"cool", 0x1F192}, {"sos", 0x1F198}, {"id", 0x1F194}, {"vs", 0x1F19A}, {"top", 0x1F51D},
    {"soon", 0x1F51C}, {"on", 0x1F51B}, {"end", 0x1F51A}, {"back", 0x1F519},
    {"arrow_up", 0x2B06}, {"arrow_down", 0x2B07}, {"arrow_left", 0x2B05}, {"arrow_right", 0x27A1},
    {"arrow_upper_right", 0x2197}, {"arrow_lower_right", 0x2198}, {"arrow_lower_left", 0x2199},
    {"arrow_upper_left", 0x2196}, {"arrow_up_down", 0x2195}, {"left_right_arrow", 0x2194},
    {"leftwards_arrow_with_hook", 0x21A9}, {"arrow_right_hook", 0x21AA},
    {"arrow_heading_up", 0x2934}, {"arrow_heading_down", 0x2935},
    {"arrows_clockwise", 0x1F503}, {"arrows_counterclockwise", 0x1F504}, {"repeat", 0x1F501},
    {"repeat_one", 0x1F502}, {"twisted_rightwards_arrows", 0x1F500},
    {"arrow_forward", 0x25B6}, {"arrow_backward", 0x25C0}, {"fast_forward", 0x23E9},
    {"rewind", 0x23EA}, {"pause_button", 0x23F8}, {"stop_button", 0x23F9},
    {"record_button", 0x23FA}, {"eject_button", 0x23CF}, {"arrow_up_small", 0x1F53C},
    {"arrow_down_small", 0x1F53D}, {"red_circle", 0x1F534}, {"large_blue_circle", 0x1F535},
    {"blue_circle", 0x1F535}, {"black_circle", 0x26AB}, {"white_circle", 0x26AA},
    {"orange_circle", 0x1F7E0}, {"yellow_circle", 0x1F7E1}, {"green_circle", 0x1F7E2},
    {"purple_circle", 0x1F7E3}, {"brown_circle", 0x1F7E4}, {"red_square", 0x1F7E5},
    {"blue_square", 0x1F7E6}, {"orange_square", 0x1F7E7}, {"yellow_square", 0x1F7E8},
    {"green_square", 0x1F7E9}, {"purple_square", 0x1F7EA}, {"brown_square", 0x1F7EB},
    {"black_large_square", 0x2B1B}, {"white_large_square", 0x2B1C},
    {"large_orange_diamond", 0x1F536}, {"large_blue_diamond", 0x1F537},
    {"small_orange_diamond", 0x1F538}, {"small_blue_diamond", 0x1F539},
    {"small_red_triangle", 0x1F53A}, {"small_red_triangle_down", 0x1F53B},
    {"diamond_shape_with_a_dot_inside", 0x1F4A0}, {"radio_button", 0x1F518},
    {"copyright", 0x00A9}, {"registered", 0x00AE}, {"tm", 0x2122}, {"infinity", 0x267E},
    {"eight_pointed_black_star", 0x2734}, {"eight_spoked_asterisk", 0x2733},
    {"sparkle", 0x2747}, {"peace_symbol", 0x262E}, {"yin_yang", 0x262F},
    {"atom_symbol", 0x269B}, {"radioactive", 0x2622}, {"biohazard", 0x2623},
    {"male_sign", 0x2642}, {"female_sign", 0x2640}, {"medical_symbol", 0x2695},
    {"wheelchair", 0x267F}, {"wavy_dash", 0x3030}, {"part_alternation_mark", 0x303D},
    {"heavy_dollar_sign", 0x1F4B2}, {"currency_exchange", 0x1F4B1},
};

// Characters whose default is text get U+FE0F after them, so they draw as
// emoji as on GitHub: below U+1F000 every one but those Unicode presents as
// emoji by default, and above it the few that default to text.
bool presentationSelector(uint32_t cp) {
    static const uint32_t kEmojiByDefault[] = {
        0x231A, 0x231B, 0x23E9, 0x23EA, 0x23F0, 0x23F3, 0x2614, 0x2615, 0x267F, 0x2693, 0x26A1,
        0x26AA, 0x26AB, 0x26BD, 0x26BE, 0x26C4, 0x26C5, 0x26D4, 0x26EA, 0x26F3, 0x26F5, 0x26FA,
        0x26FD, 0x2705, 0x270A, 0x270B, 0x2728, 0x274C, 0x274E, 0x2753, 0x2754, 0x2755, 0x2757,
        0x2795, 0x2796, 0x2797, 0x27B0, 0x27BF, 0x2B1B, 0x2B1C, 0x2B50, 0x2B55,
    };
    static const uint32_t kTextByDefault[] = {
        0x1F321, 0x1F327, 0x1F32A, 0x1F32B, 0x1F336, 0x1F399, 0x1F39E, 0x1F39F, 0x1F3CD,
        0x1F3CE, 0x1F3D5, 0x1F3D6, 0x1F3DC, 0x1F3DD, 0x1F3F3, 0x1F3F7, 0x1F43F, 0x1F441,
        0x1F54A, 0x1F56F, 0x1F573, 0x1F575, 0x1F576, 0x1F577, 0x1F579, 0x1F587, 0x1F58A,
        0x1F58B, 0x1F58C, 0x1F58D, 0x1F5A5, 0x1F5A8, 0x1F5B1, 0x1F5B2, 0x1F5C2, 0x1F5C3,
        0x1F5C4, 0x1F5D1, 0x1F5D2, 0x1F5D3, 0x1F5DC, 0x1F5DD, 0x1F5E1, 0x1F5F3, 0x1F5FA,
        0x1F6CB, 0x1F6CF, 0x1F6E0, 0x1F6E1, 0x1F6E9, 0x1F6F0,
    };
    if (cp < 0x1F000) {
        for (uint32_t e : kEmojiByDefault)
            if (e == cp) return false;
        return true;
    }
    for (uint32_t t : kTextByDefault)
        if (t == cp) return true;
    return false;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

const std::unordered_map<std::string, uint32_t>& table() {
    static const std::unordered_map<std::string, uint32_t> map = [] {
        std::unordered_map<std::string, uint32_t> m;
        for (const Entry& e : kTable) m.emplace(e.name, e.cp);
        return m;
    }();
    return map;
}

bool nameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
}

bool alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

}  // namespace

namespace MarkdownEmoji {

std::string lookup(const std::string& name) {
    const auto& m = table();
    const auto it = m.find(name);
    if (it == m.end()) return "";
    std::string out;
    appendUtf8(out, it->second);
    if (presentationSelector(it->second)) appendUtf8(out, 0xFE0F);
    return out;
}

std::string replace(const std::string& text) {
    if (text.find(':') == std::string::npos) return text;
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != ':' || (i > 0 && alnum(text[i - 1]))) {
            out += text[i++];
            continue;
        }
        size_t j = i + 1;
        while (j < text.size() && j - i <= 40 && nameChar(text[j])) j++;
        if (j < text.size() && text[j] == ':' && j > i + 1 &&
            (j + 1 >= text.size() || !alnum(text[j + 1]))) {
            const std::string emoji = lookup(text.substr(i + 1, j - i - 1));
            if (!emoji.empty()) {
                out += emoji;
                i = j + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

size_t count() { return table().size(); }

}  // namespace MarkdownEmoji
