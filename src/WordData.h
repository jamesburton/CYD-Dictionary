#pragma once

// ==============================================================================
// Embedded starter dictionary (v1).
// A curated, child-friendly word set kept in flash so the device works with no
// SD card. Iteration #2 replaces this with the full Wordset corpus streamed from
// an SD card via an on-disk index (see docs/superpowers/specs/).
//
// IMPORTANT: keep this list sorted ascending by `term` (lowercase). Browse and
// search assume alphabetical order.
// ==============================================================================

struct Word
{
    const char* term;
    const char* pos;       // part of speech
    const char* def;       // simple, child-friendly definition
    const char* example;   // example sentence ("" if none)
};

static const Word WORDS[] = {
    {"actor",     "noun",      "A person who performs in plays, films, or TV.", "The actor played a brave knight."},
    {"adventure", "noun",      "An exciting trip where something unusual happens.", "They went on an adventure in the woods."},
    {"animal",    "noun",      "A living thing that can move and is not a plant.", "A dog is my favourite animal."},
    {"apple",     "noun",      "A round fruit that is red, green, or yellow.", "She ate a juicy apple."},
    {"balloon",   "noun",      "A bag of thin rubber filled with air or gas.", "The red balloon floated away."},
    {"beach",     "noun",      "Sandy or stony land beside the sea.", "We built a sandcastle on the beach."},
    {"bicycle",   "noun",      "A vehicle with two wheels that you push with your feet.", "He rode his bicycle to school."},
    {"brave",     "adjective", "Ready to face danger or pain without being too scared.", "The brave girl saved the kitten."},
    {"bridge",    "noun",      "A structure built to go over a river or road.", "We walked across the wooden bridge."},
    {"bubble",    "noun",      "A round ball of air inside liquid.", "She blew a giant soap bubble."},
    {"butterfly", "noun",      "An insect with large, often colourful wings.", "A butterfly landed on the flower."},
    {"castle",    "noun",      "A large strong building where kings and queens lived long ago.", "The castle had tall towers."},
    {"cloud",     "noun",      "A white or grey shape made of tiny drops of water in the sky.", "That cloud looks like a sheep."},
    {"courage",   "noun",      "The strength to do something even when it is scary.", "It took courage to speak on stage."},
    {"curious",   "adjective", "Wanting to learn or know about things.", "The curious cat looked in the box."},
    {"dance",     "verb",      "To move your body in time with music.", "They love to dance at parties."},
    {"dinosaur",  "noun",      "A huge animal that lived millions of years ago.", "The dinosaur had sharp teeth."},
    {"dolphin",   "noun",      "A clever sea animal that breathes air.", "A dolphin jumped beside the boat."},
    {"dragon",    "noun",      "A make-believe animal that can often breathe fire.", "The dragon guarded the gold."},
    {"dream",     "noun",      "Pictures and stories in your mind while you sleep.", "I had a happy dream last night."},
    {"eagle",     "noun",      "A large strong bird that hunts other animals.", "The eagle soared high above."},
    {"explore",   "verb",      "To travel around a place to learn about it.", "We will explore the cave tomorrow."},
    {"flower",    "noun",      "The colourful part of a plant that makes seeds.", "She picked a yellow flower."},
    {"forest",    "noun",      "A large area covered with many trees.", "Owls live deep in the forest."},
    {"friend",    "noun",      "A person you like and enjoy being with.", "My best friend shares her toys."},
    {"galaxy",    "noun",      "A huge group of stars, gas, and dust in space.", "Our galaxy is the Milky Way."},
    {"garden",    "noun",      "A piece of land where plants are grown.", "Carrots grow in our garden."},
    {"gentle",    "adjective", "Kind and careful, not rough.", "Be gentle with the baby rabbit."},
    {"giant",     "noun",      "A make-believe person who is very, very big.", "The giant had enormous feet."},
    {"glitter",   "verb",      "To shine with many tiny flashes of light.", "The snow seemed to glitter."},
    {"happy",     "adjective", "Feeling pleased and full of joy.", "She was happy to see her gran."},
    {"harvest",   "noun",      "The gathering of crops that are ready to eat.", "Autumn is the time for harvest."},
    {"helmet",    "noun",      "A hard hat that protects your head.", "Always wear a helmet when cycling."},
    {"honey",     "noun",      "A sweet sticky food made by bees.", "He spread honey on his toast."},
    {"island",    "noun",      "Land with water all around it.", "A small island sat in the lake."},
    {"jungle",    "noun",      "A thick forest in a hot country.", "Monkeys swing through the jungle."},
    {"kangaroo",  "noun",      "An animal from Australia that hops on big back legs.", "The kangaroo carried a joey."},
    {"kind",      "adjective", "Friendly, caring, and helpful to others.", "It was kind of you to help."},
    {"kitten",    "noun",      "A baby cat.", "The kitten chased a ball of wool."},
    {"ladder",    "noun",      "A set of steps used to climb up high.", "He climbed the ladder to the roof."},
    {"lantern",   "noun",      "A light inside a case that you can carry.", "The lantern lit up the path."},
    {"library",   "noun",      "A place full of books you can read or borrow.", "We borrowed three books from the library."},
    {"lightning", "noun",      "A bright flash of electricity in the sky.", "Lightning lit up the dark clouds."},
    {"lion",      "noun",      "A big wild cat known as king of the jungle.", "The lion gave a mighty roar."},
    {"magnet",    "noun",      "A piece of metal that pulls iron towards it.", "The magnet stuck to the fridge."},
    {"meadow",    "noun",      "A field full of grass and wild flowers.", "Cows grazed in the green meadow."},
    {"mountain",  "noun",      "A very high hill made of rock.", "Snow covered the tall mountain."},
    {"music",     "noun",      "Pleasant sounds made by voices or instruments.", "We danced to happy music."},
    {"nature",    "noun",      "Everything in the world not made by people.", "We study nature on our walks."},
    {"nest",      "noun",      "A home that birds build for their eggs.", "Three eggs sat in the nest."},
    {"ocean",     "noun",      "A very large area of salty water.", "Whales live in the deep ocean."},
    {"orange",    "noun",      "A round juicy fruit with a thick skin.", "I peeled a sweet orange."},
    {"owl",       "noun",      "A bird with big eyes that hunts at night.", "The owl hooted in the dark."},
    {"parade",    "noun",      "A line of people walking to celebrate something.", "We watched the colourful parade."},
    {"penguin",   "noun",      "A black-and-white bird that swims but cannot fly.", "The penguin slid on the ice."},
    {"planet",    "noun",      "A large round object that moves around a star.", "Earth is the planet we live on."},
    {"puzzle",    "noun",      "A game or problem you solve by thinking.", "She finished the jigsaw puzzle."},
    {"rainbow",   "noun",      "A curved band of colours in the sky after rain.", "A rainbow appeared after the storm."},
    {"river",     "noun",      "A long stream of water that flows to the sea.", "Fish swam in the clear river."},
    {"robot",     "noun",      "A machine that can do jobs by itself.", "The robot tidied the room."},
    {"rocket",    "noun",      "A vehicle that flies into space.", "The rocket blasted off."},
    {"sail",      "verb",      "To travel across water in a boat.", "They will sail to the island."},
    {"school",    "noun",      "A place where children go to learn.", "We learn maths at school."},
    {"shadow",    "noun",      "The dark shape made when something blocks light.", "My shadow was long at sunset."},
    {"sketch",    "verb",      "To draw something quickly and simply.", "He likes to sketch birds."},
    {"smile",     "verb",      "To turn up your mouth to show you are happy.", "She gave a big smile."},
    {"snail",     "noun",      "A small slow animal with a shell on its back.", "A snail crept up the wall."},
    {"sparkle",   "verb",      "To shine with bright moving points of light.", "The stars sparkle at night."},
    {"spider",    "noun",      "A small animal with eight legs that spins webs.", "A spider spun a silky web."},
    {"square",    "noun",      "A shape with four equal straight sides.", "Draw a square on the page."},
    {"storm",     "noun",      "Very bad weather with strong wind and rain.", "The storm shook the trees."},
    {"sunshine",  "noun",      "The light and warmth that comes from the sun.", "We played in the sunshine."},
    {"telescope", "noun",      "A tube you look through to see far-away things.", "We saw the moon through a telescope."},
    {"thunder",   "noun",      "The loud noise that comes after lightning.", "Thunder rumbled in the distance."},
    {"tiger",     "noun",      "A large wild cat with orange fur and black stripes.", "The tiger hid in the long grass."},
    {"treasure",  "noun",      "Gold, jewels, or other valuable things.", "They dug up buried treasure."},
    {"turtle",    "noun",      "A slow animal with a hard shell that can swim.", "The turtle swam to the rock."},
    {"umbrella",  "noun",      "A cloth cover on a stick that keeps off the rain.", "She opened her umbrella."},
    {"valley",    "noun",      "Low land between hills or mountains.", "A river ran through the valley."},
    {"village",   "noun",      "A small group of houses in the countryside.", "Grandpa lives in a tiny village."},
    {"volcano",   "noun",      "A mountain that can shoot out hot melted rock.", "The volcano sent up smoke."},
    {"voyage",    "noun",      "A long journey, usually by sea or in space.", "The ship began its voyage."},
    {"waterfall", "noun",      "Water that falls down from a high place.", "We swam below the waterfall."},
    {"whale",     "noun",      "The largest animal that lives in the sea.", "The whale spouted water."},
    {"whisper",   "verb",      "To speak very quietly.", "She whispered a secret."},
    {"window",    "noun",      "An opening in a wall, usually filled with glass.", "Rain tapped on the window."},
    {"wizard",    "noun",      "A make-believe man who can do magic.", "The wizard waved his wand."},
    {"wonder",    "verb",      "To think about something with curiosity.", "I wonder how birds fly."},
    {"yawn",      "verb",      "To open your mouth wide when tired or bored.", "He gave a sleepy yawn."},
    {"zebra",     "noun",      "A wild horse-like animal with black-and-white stripes.", "The zebra ran across the plain."},
};

static const int WORD_COUNT = sizeof(WORDS) / sizeof(WORDS[0]);
