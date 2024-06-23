parse_key() {
  local data=$(echo "$1" | grep "$2" -A $3 | grep -v "$2")
  local norm=$(echo "$data" | sed -n 's/ - /&\n/;s/.*\n//p' | cut -c-47)
  echo "$norm"
}

parse_attr() {
  local x=$(echo "$1" | grep "$2")
  local y="(?<=${2}:)[A-Za-z0-9]*"
  local z=$(echo "$x" | grep -P "$y" | sed 's/.*\://' | sed 's/^[[:blank:]]*//;s/[[:blank:]]*$//')
  echo "$z"
}


