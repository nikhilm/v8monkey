/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

/* File locked complete MAR file patch apply failure test */

const TEST_ID = "0170";

// The files are in the same order as they are applied from the mar
const TEST_FILES = [
{
  fileName         : "00png0.png",
  relPathDir       : "0/00/",
  originalContents : null,
  compareContents  : null,
  originalFile     : "data/partial.png",
  compareFile      : "data/partial.png"
}, {
  fileName         : "00text0",
  relPathDir       : "0/00/",
  originalContents : "ShouldNotBeReplaced\n",
  compareContents  : "ShouldNotBeReplaced\n",
  originalFile     : null,
  compareFile      : null
}, {
  fileName         : "00text1",
  relPathDir       : "0/00/",
  originalContents : "ShouldNotBeReplaced\n",
  compareContents  : "ShouldNotBeReplaced\n",
  originalFile     : null,
  compareFile      : null
}, {
  fileName         : "0exe0.exe",
  relPathDir       : "0/",
  originalContents : null,
  compareContents  : null,
  originalFile     : "data/partial.png",
  compareFile      : "data/partial.png"
}, {
  fileName         : "10text0",
  relPathDir       : "1/10/",
  originalContents : "ShouldNotBeReplaced\n",
  compareContents  : "ShouldNotBeReplaced\n",
  originalFile     : null,
  compareFile      : null
}, {
  fileName         : "exe0.exe",
  relPathDir       : "",
  originalContents : null,
  compareContents  : null,
  originalFile     : "data/partial.png",
  compareFile      : "data/partial.png"
}];

function run_test() {
  if (!IS_WIN || IS_WINCE) {
    logTestInfo("this test is only applicable to Windows... returning early");
    return;
  }

  do_test_pending();
  do_register_cleanup(cleanupUpdaterTest);

  setupUpdaterTest(MAR_COMPLETE_FILE);

  // Exclusively lock an existing file so it is in use during the update
  let helperBin = do_get_file(HELPER_BIN_FILE);
  let applyToDir = getApplyDirFile();
  helperBin.copyTo(applyToDir, HELPER_BIN_FILE);
  helperBin = getApplyDirFile(HELPER_BIN_FILE);
  let lockFileRelPath = TEST_FILES[3].relPathDir + TEST_FILES[3].fileName;
  let args = [getApplyDirPath(), "input", "output", "-s", "20", lockFileRelPath];
  let lockFileProcess = AUS_Cc["@mozilla.org/process/util;1"].
                     createInstance(AUS_Ci.nsIProcess);
  lockFileProcess.init(helperBin);
  lockFileProcess.run(false, args, args.length);

  do_timeout(TEST_HELPER_TIMEOUT, waitForHelperSleep);
}

function doUpdate() {
  // apply the complete mar
  let exitValue = runUpdate();
  logTestInfo("testing updater binary process exitValue for success when " +
              "applying a complete mar");
  do_check_eq(exitValue, 0);

  setupHelperFinish();
}

function checkUpdate() {
  let updatesDir = do_get_file(TEST_ID + UPDATES_DIR_SUFFIX);
  let applyToDir = getApplyDirFile();

  logTestInfo("testing update.status should be " + STATE_FAILED);
  // The update status format for a failure is failed: # where # is the error
  // code for the failure.
  do_check_eq(readStatusFile(updatesDir).split(": ")[0], STATE_FAILED);

  checkFilesAfterUpdateFailure();

  logTestInfo("testing tobedeleted directory doesn't exist");
  let toBeDeletedDir = applyToDir.clone();
  toBeDeletedDir.append("tobedeleted");
  do_check_false(toBeDeletedDir.exists());

  checkCallbackAppLog();
}
