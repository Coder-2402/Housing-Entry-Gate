function doGet(e) {
  var action = e.parameter.action || "log";
  var ss = SpreadsheetApp.getActiveSpreadsheet();

  if (action === "getCards") {
    // Ambil daftar kartu dari sheet KARTU (kolom A, mulai baris 2)
    var kartuSheet = ss.getSheetByName("KARTU");
    if (!kartuSheet) {
      return ContentService
        .createTextOutput("")
        .setMimeType(ContentService.MimeType.TEXT);
    }

    var lastRow = kartuSheet.getLastRow();
    if (lastRow < 2) {
      return ContentService
        .createTextOutput("")
        .setMimeType(ContentService.MimeType.TEXT);
    }

    var range = kartuSheet.getRange(2, 1, lastRow - 1, 1);
    var values = range.getValues();
    var out = "";

    for (var i = 0; i < values.length; i++) {
      var uid = (values[i][0] + "").trim().toUpperCase();
      if (uid) {
        out += uid + "\n";  // satu UID per baris
      }
    }

    return ContentService
      .createTextOutput(out)
      .setMimeType(ContentService.MimeType.TEXT);
  }

  // Default: LOG
  var uid = e.parameter.uid || "";
  var status = e.parameter.status || "";

  if (!uid) {
    return ContentService
      .createTextOutput("NO_UID")
      .setMimeType(ContentService.MimeType.TEXT);
  }

  var logSheet = ss.getSheetByName("LOG");
  if (!logSheet) {
    logSheet = ss.insertSheet("LOG");
  }

  var now = new Date();
  logSheet.appendRow([
    now,
    uid,
    status,
    ""
  ]);

  return ContentService
    .createTextOutput("OK")
    .setMimeType(ContentService.MimeType.TEXT);
}
