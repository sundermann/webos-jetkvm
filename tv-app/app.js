(function () {
  "use strict";
  var service = "luna://org.webosbrew.jetkvm.service";
  var password = document.getElementById("password");
  var autostart = document.getElementById("autostart");
  var resolution = document.getElementById("resolution");
  var resolutionDirty = false;
  resolution.addEventListener("change", function () {
    resolutionDirty = true;
  });
  var status = document.getElementById("status");
  var busy = false;
  function call(uri, method, parameters) {
    return new Promise(function (resolve, reject) {
      webOS.service.request(uri, {
        method: method,
        parameters: parameters || {},
        onSuccess: resolve,
        onFailure: function (error) {
          reject(new Error(error.errorText || "TV service request failed"));
        },
      });
    });
  }
  function controls(running) {
    password.disabled = busy;
    autostart.disabled = busy;
    resolution.disabled = busy;
    document.getElementById("start").disabled = busy || running;
    document.getElementById("stop").disabled = busy || !running;
    document.getElementById("save").disabled = busy;
  }
  function refresh() {
    return call(service, "status").then(function (value) {
      document.getElementById("state").textContent = value.streaming
        ? "Viewer connected"
        : value.ready
          ? "Server ready"
          : value.running
            ? "Starting…"
            : "Stopped";
      if (value.urls.length) {
        document.getElementById("urls").textContent = value.urls.join("\n");
      }
      password.placeholder = value.settings.passwordConfigured
        ? "Leave blank to keep the saved password"
        : "Enter a password";
      document.getElementById("password-hint").textContent = value.settings.passwordConfigured
        ? "A browser password is saved."
        : "Set a password to enable browser access.";
      if (!resolution.options.length) {
        value.videoPresets.forEach(function (preset) {
          var option = document.createElement("option");
          option.value = preset.id;
          option.textContent =
            preset.id + " · " + preset.width + " × " + preset.height + " · " + preset.fps + " fps";
          resolution.appendChild(option);
        });
      }
      if (!resolutionDirty) {
        resolution.value = value.settings.resolution;
      }
      var preset = value.videoPresets.filter(function (item) {
        return item.id === value.settings.resolution;
      })[0];
      if (preset) {
        document.getElementById("video-format").textContent =
          "Hardware H.264 · " + preset.width + " × " + preset.height + " · " + preset.fps + " fps";
      }
      controls(value.running);
      return value;
    });
  }
  function perform(action) {
    if (busy) {
      return;
    }
    busy = true;
    controls(false);
    var operation =
      action === "save"
        ? call(service, "configure", {
            password: password.value,
            autostart: autostart.checked,
            resolution: resolution.value,
          })
        : call(service, action);
    return operation
      .then(function (value) {
        password.value = "";
        if (action === "save") {
          resolutionDirty = false;
        }
        status.textContent = value.message || "Done";
        return value;
      })
      .catch(function (error) {
        status.textContent = error.message;
      })
      .then(function () {
        busy = false;
        return refresh();
      })
      .catch(function (error) {
        status.textContent = error.message;
      });
  }
  ["save", "start", "stop"].forEach(function (action) {
    document.getElementById(action).addEventListener("click", function () {
      perform(action);
    });
  });
  document.addEventListener("keydown", function (event) {
    if (event.target === resolution && (event.keyCode === 37 || event.keyCode === 39)) {
      resolution.selectedIndex =
        (resolution.selectedIndex + (event.keyCode === 39 ? 1 : -1) + resolution.options.length) %
        resolution.options.length;
      resolutionDirty = true;
      event.preventDefault();
      return;
    }
    if (event.keyCode !== 38 && event.keyCode !== 40) {
      return;
    }
    var elements = Array.prototype.filter.call(
      document.querySelectorAll("input,select,button"),
      function (element) {
        return !element.disabled;
      },
    );
    var current = elements.indexOf(document.activeElement);
    var next = (current + (event.keyCode === 40 ? 1 : -1) + elements.length) % elements.length;
    elements[next].focus();
    event.preventDefault();
  });
  refresh()
    .then(function (value) {
      if (value.root) {
        return value;
      }
      status.textContent = "Enabling root access through Homebrew Channel…";
      return call("luna://org.webosbrew.hbchannel.service", "exec", {
        command:
          "/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service org.webosbrew.jetkvm.service && (pkill -x -f /media/developer/apps/usr/palm/services/org.webosbrew.jetkvm.service/webos-jetkvm-service || true)",
      }).then(function () {
        return refresh();
      });
    })
    .then(function (value) {
      if (!value.root) {
        throw new Error(
          "Root access is required. Check that Homebrew Channel is installed and the TV is rooted.",
        );
      }
      autostart.checked = value.settings.autostart;
      status.textContent =
        "Save a password, start the server, then open the address above in Firefox.";
      if (value.settings.passwordConfigured) {
        resolution.focus();
      } else {
        password.focus();
      }
      setInterval(function () {
        if (!busy) {
          refresh().catch(function () {});
        }
      }, 2000);
      return value;
    })
    .catch(function (error) {
      status.textContent =
        error.message + "\nCheck that the TV is rooted and Homebrew Channel is installed.";
    });
})();
