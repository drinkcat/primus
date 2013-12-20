
data = File.open("/usr/include/GLES3/gl3.h").to_a.join.gsub(/[\n\t]/, ' ').split(/;/)

data.each{ |statement|
  puts statement
#  next if (!statement.match(/^ *EGLAPI.*/))
  next if (!statement.match(/^ *GL_APICALL.*/))
  statement.gsub!(/ +/, ' ')
  statement.gsub!(/^ /, '')
  #fields = statement.split(' ')
  #retval = fields[1]
  #puts fields[3..fields.length-1].join(' ').match(/.*(.*)/).inspect

  match = statement.match(/[^ ]* +([^ ]*) +[^ ]* +([^ ]*) *\((.*)\)/)
  #puts match.inspect
  retval = match[1]
  func = match[2]
  params = match[3]

  extra = ''
  if (params != "void") then
    p = params.split(/, */).map{|k| k.split(/ /)}
    if (p[0][0] == "EGLDisplay") then
      p = p[1..(p.length-1)]
    end
    if (p.length > 0) then
      extra = "," + p.map{|x| x[1]}.join(',')
    end
  end

  puts "DEF_EGL_PROTO(#{retval}, #{func}, (#{params})#{extra})"
};
