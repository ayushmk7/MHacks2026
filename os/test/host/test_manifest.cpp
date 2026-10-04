// LINK: src/vk/host/manifest.cpp
// test_manifest - the parser for the two app.ini keys Badge OS adds (docs/os/platform/app-host.md,
// "Manifest"): both keys, defaults, spaces, comments, unknown keys ignored, bad min_api.
#include <Arduino.h>

#include <stdio.h>

#include "../../src/vk/host/manifest.h"

using vk::host::manifest::Extra;
using vk::host::manifest::parse;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// Parses `text` into an Extra that starts with other values, so a test sees the defaults being set.
static bool parsed(const char *text, Extra &out) {
  out.permissions = "stale";
  out.min_api = 77;
  return parse(String(text), out);
}

static void test_both_keys() {
  Extra extra;
  // The example of app-host.md.
  CHECK(parsed("name=Pay\n"
               "version=1.0.0\n"
               "author=team\n"
               "description=Pay a nearby badge\n"
               "permissions=sign,net,espnow\n"
               "min_api=2\n",
               extra));
  CHECK(extra.permissions == "sign,net,espnow");
  CHECK(extra.min_api == 2);

  // Any order, and no newline at the end of the file.
  CHECK(parsed("min_api=3\npermissions=history", extra));
  CHECK(extra.permissions == "history");
  CHECK(extra.min_api == 3);

  // CRLF line ends.
  CHECK(parsed("name=Pay\r\npermissions=sign,net\r\nmin_api=2\r\n", extra));
  CHECK(extra.permissions == "sign,net");
  CHECK(extra.min_api == 2);

  // The key in any case, as upstream reads its own keys; names keep theirs.
  CHECK(parsed("Permissions=sign\nMIN_API=2\n", extra));
  CHECK(extra.permissions == "sign");
  CHECK(extra.min_api == 2);
  CHECK(parsed("permissions=Sign\n", extra));
  CHECK(extra.permissions == "Sign");

  // The last line of a repeated key wins.
  CHECK(parsed("permissions=sign\nmin_api=1\npermissions=net\nmin_api=2\n", extra));
  CHECK(extra.permissions == "net");
  CHECK(extra.min_api == 2);
}

static void test_defaults() {
  Extra fresh;
  CHECK(fresh.permissions == "");
  CHECK(fresh.min_api == 1);

  Extra extra;
  CHECK(parsed("", extra));                               // an empty file
  CHECK(extra.permissions == "" && extra.min_api == 1);

  CHECK(parsed("name=Hello\nversion=1.0.0\n", extra));    // an upstream app: neither key
  CHECK(extra.permissions == "" && extra.min_api == 1);

  CHECK(parsed("permissions=sign\n", extra));             // only one of them
  CHECK(extra.permissions == "sign" && extra.min_api == 1);
  CHECK(parsed("min_api=2\n", extra));
  CHECK(extra.permissions == "" && extra.min_api == 2);

  CHECK(parsed("permissions=\n", extra));                 // T-APP7: an empty list is a list of none
  CHECK(extra.permissions == "" && extra.min_api == 1);

  CHECK(parsed("min_api=0\n", extra));                    // an integer, and no higher than any API
  CHECK(extra.min_api == 0);
  CHECK(parsed("min_api=007\n", extra));
  CHECK(extra.min_api == 7);
}

static void test_spaces() {
  Extra extra;
  CHECK(parsed("  permissions = sign,net  \n\tmin_api =  2\t\n", extra));
  CHECK(extra.permissions == "sign,net");
  CHECK(extra.min_api == 2);

  // Spaces and empty items inside the list are dropped; the order is kept.
  CHECK(parsed("permissions= sign , net,,espnow ,\n", extra));
  CHECK(extra.permissions == "sign,net,espnow");
  CHECK(parsed("permissions= , ,\n", extra));
  CHECK(extra.permissions == "");

  // Blank lines and lines of spaces between the keys.
  CHECK(parsed("\n\n   \npermissions=mic\n\n \t \nmin_api=2\n\n", extra));
  CHECK(extra.permissions == "mic");
  CHECK(extra.min_api == 2);
}

static void test_comments() {
  Extra extra;
  CHECK(parsed("# permissions=sign\n"
               "; min_api=9\n"
               "  # permissions=net\n"
               "permissions=storage\n",
               extra));
  CHECK(extra.permissions == "storage");
  CHECK(extra.min_api == 1);

  // A '#' inside a value is part of the value, as in upstream's parser.
  CHECK(parsed("permissions=sign#net\n", extra));
  CHECK(extra.permissions == "sign#net");
}

static void test_unknown_keys_ignored() {
  Extra extra;
  CHECK(parsed("name=permissions=sign\n"          // the key is `name`
               "permission=sign\n"                // not `permissions`
               "xpermissions=sign\n"
               "min_api2=9\n"
               "entry=main.lua\n"
               "colour=green\n"
               "no separator here\n"
               "=sign\n"                          // no key
               "permissions.extra=net\n",
               extra));
  CHECK(extra.permissions == "");
  CHECK(extra.min_api == 1);

  CHECK(parsed("junk\npermissions=ble\nmore junk = 4\nmin_api=2\n", extra));
  CHECK(extra.permissions == "ble");
  CHECK(extra.min_api == 2);
}

static void test_bad_min_api() {
  Extra extra;
  const char *bad[] = {
      "min_api=\n",        "min_api=abc\n", "min_api=2x\n",  "min_api=-1\n", "min_api=+2\n",
      "min_api=1.5\n",     "min_api=0x2\n", "min_api=2 3\n", "min_api=two\n",
  };
  for (const char *text : bad) {
    const bool ok = parsed(text, extra);
    if (ok) printf("accepted: %s", text);
    CHECK(!ok);
    CHECK(extra.min_api == 0xFFFFFFFFu);          // a caller that ignores the result still refuses the app
  }

  // The permissions of such a file are still read.
  CHECK(!parsed("permissions=sign\nmin_api=abc\n", extra));
  CHECK(extra.permissions == "sign");

  // A later good line replaces a bad one, and the other way round.
  CHECK(parsed("min_api=abc\nmin_api=2\n", extra));
  CHECK(extra.min_api == 2);
  CHECK(!parsed("min_api=2\nmin_api=abc\n", extra));

  // Large numbers: the largest value fits, anything above it is stored as the largest.
  CHECK(parsed("min_api=99\n", extra));
  CHECK(extra.min_api == 99);
  CHECK(parsed("min_api=4294967295\n", extra));
  CHECK(extra.min_api == 0xFFFFFFFFu);
  CHECK(parsed("min_api=4294967296\n", extra));
  CHECK(extra.min_api == 0xFFFFFFFFu);
  CHECK(parsed("min_api=99999999999999999999999999999999\n", extra));
  CHECK(extra.min_api == 0xFFFFFFFFu);
}

// The launcher keys: category (a folder name, lower case) and hidden.
static void test_launcher_keys() {
  using vk::host::manifest::Launcher;
  using vk::host::manifest::parseLauncher;
  Launcher l;
  l.category = "stale";
  l.hidden = true;
  CHECK(parseLauncher(String("name=Dice\npermissions=\n"), l));
  CHECK(l.category == "");
  CHECK(!l.hidden);

  CHECK(parseLauncher(String("category = Games\r\nhidden=1\n"), l));
  CHECK(l.category == "games");
  CHECK(l.hidden);
  CHECK(parseLauncher(String("hidden=true\n"), l));
  CHECK(l.hidden);
  CHECK(parseLauncher(String("hidden=0\n"), l));
  CHECK(!l.hidden);
  CHECK(parseLauncher(String("hidden=yes\n"), l));   // only 1 and true count
  CHECK(!l.hidden);

  // A name that is not [a-z0-9_-] or is longer than 16 characters is no folder at all.
  CHECK(parseLauncher(String("category=my games\n"), l));
  CHECK(l.category == "");
  CHECK(parseLauncher(String("category=../x\n"), l));
  CHECK(l.category == "");
  CHECK(parseLauncher(String("category=abcdefghijklmnopq\n"), l));
  CHECK(l.category == "");
  CHECK(parseLauncher(String("category=dev_tools-2\n"), l));
  CHECK(l.category == "dev_tools-2");
  CHECK(!l.countNotes);

  // count: only "notes" is known.
  CHECK(parseLauncher(String("count=notes\n"), l));
  CHECK(l.countNotes);
  CHECK(parseLauncher(String("count=apples\n"), l));
  CHECK(!l.countNotes);

  // The keys push-apps.sh reads are not the firmware's business, and do not disturb the others.
  CHECK(parseLauncher(String("profile=dev\ninclude=game\ncategory=tests\n"), l));
  CHECK(l.category == "tests");
  Extra extra;
  CHECK(parsed("category=games\nhidden=1\npermissions=sign\n", extra));
  CHECK(extra.permissions == "sign");
}

int main() {
  test_launcher_keys();
  test_both_keys();
  test_defaults();
  test_spaces();
  test_comments();
  test_unknown_keys_ignored();
  test_bad_min_api();

  if (fails) {
    printf("%d manifest checks failed\n", fails);
    return 1;
  }
  printf("all manifest tests passed\n");
  return 0;
}
