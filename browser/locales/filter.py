def test(mod, path, entity = None):
  import re
  # ignore anyhting but Firefox
  if mod not in ("netwerk", "dom", "toolkit", "security/manager",
                 "browser", "extensions/reporter", "extensions/spellcheck",
                 "other-licenses/branding/firefox"):
    return False
  if mod not in ["browser", "extensions/spellcheck", 
                 "other-licenses/branding/firefox"]:
    # we only have exceptions for browser and extensions/spellcheck
    # and other-licenses/branding/firefox
    return True
  if not entity:
    if mod == "extensions/spellcheck":
      return False
    # browser
    return not (re.match(r"searchplugins\/.+\.xml", path) or
                re.match(r"chrome\/help\/images\/[A-Za-z-_]+\.png", path))
  if mod == "extensions/spellcheck":
    # l10n ships en-US dictionary or something, do compare
    return True
  if path == "defines.inc":
    return entity != "MOZ_LANGPACK_CONTRIBUTORS"

  if mod == "browser":
    if path == "chrome/browser-region/region.properties":
  
      return not (re.match(r"browser\.search\.order.[1-9]", entity) or
                  re.match(r"browser\.contentHandlers\.types.[0-5]", entity))
    return not path == "chrome/browser/preferences/tabs.dtd" or \
        not entity == "brandDTD"

  if path == "brand.properties":
    return not (re.match(r"homePageMultipleStart[123](Label|URL)", entity) or
                re.match(r"homePageMultipleStartMain", entity) or
                re.match(r"homePageOptionCount", entity))
