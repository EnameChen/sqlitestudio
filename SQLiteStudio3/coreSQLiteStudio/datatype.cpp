#include "datatype.h"
#include <QMetaEnum>
#include <QRegularExpression>

QList<DataType::Enum> DataType::values = []() -> QList<DataType::Enum>
{
    QList<DataType::Enum> list;
    QMetaEnum metaEnum = DataType::staticMetaObject.enumerator(0);
    DataType::Enum value;
    for (int i = 0; i < metaEnum.keyCount(); i++)
    {
        value = static_cast<DataType::Enum>(metaEnum.value(i));
        if (value == DataType::unknown)
            continue;

        list << value;
    }

    return list;
}();

const QStringList DataType::names = []() -> QStringList
{
    QStringList list;
    QMetaEnum metaEnum = DataType::staticMetaObject.enumerator(0);
    DataType::Enum value;
    for (int i = 0; i < metaEnum.keyCount(); i++)
    {
        value = static_cast<DataType::Enum>(metaEnum.value(i));
        if (value == DataType::unknown)
            continue;

        list << DataType::toString(value);
    }

    return list;
}();

QList<DataType::Enum> DataType::valuesForUiDropdown = {BLOB, INTEGER, NUMERIC, REAL, TEXT};

DataType::DataType()
{
    setEmpty();
}

DataType::DataType(const QString& fullTypeString)
{
    static const QRegularExpression
            re(R"(^(?<type>[^\)]*)\s*(\((?<scale>[\d\.]+)\s*(,\s*(?<precision>[\d\.])+\s*)?\))?$)");

    QRegularExpressionMatch match = re.match(fullTypeString);
    if (!match.hasMatch())
    {
        setEmpty();
        return;
    }

    typeStr = match.captured("type");
    type = fromString(typeStr, Qt::CaseInsensitive);
    precision = match.captured("precision");
    scale = match.captured("scale");
}

DataType::DataType(const QString& type, const QVariant& scale, const QVariant& precision)
{
    this->type = fromString(type, Qt::CaseInsensitive);
    this->typeStr = type;
    this->precision = precision;
    this->scale = scale;
}

DataType::DataType(const DataType& other) :
    QObject()
{
    operator=(other);
}

void DataType::setEmpty()
{
    type = ::DataType::unknown;
    typeStr = "";
    precision = QVariant();
    scale = QVariant();
}

DataType::Enum DataType::getType() const
{
    return type;
}

void DataType::setType(DataType::Enum value)
{
    type = value;
    typeStr = toString(type);
}

QVariant DataType::getPrecision() const
{
    return precision;
}

void DataType::setPrecision(const QVariant& value)
{
    precision = value;
}

QVariant DataType::getScale() const
{
    return scale;
}

void DataType::setScale(const QVariant& value)
{
    scale = value;
}

QString DataType::toString() const
{
    return typeStr;
}

QString DataType::toFullTypeString() const
{
    QString str = typeStr;
    if (!precision.isNull())
    {
        if (!scale.isNull())
            str += " ("+scale.toString()+", "+precision.toString()+")";
        else
            str += " ("+scale.toString()+")";
    }
    return str;
}

bool DataType::isNumeric()
{
    return isNumeric(type);
}

bool DataType::isBinary()
{
    return isBinary(typeStr);
}

bool DataType::isNull()
{
    return type == ::DataType::unknown;
}

bool DataType::isEmpty()
{
    return typeStr.isEmpty();
}

DataType& DataType::operator=(const DataType& other)
{
    this->type = other.type;
    this->typeStr = other.typeStr;
    this->precision = other.precision;
    this->scale = scale;
    return *this;
}

QString DataType::toString(DataType::Enum e)
{
    QMetaEnum metaEnum = staticMetaObject.enumerator(0);
    const char* key = metaEnum.valueToKey(e);
    if (!key)
        return QString();

    return key;
}

DataType::Enum DataType::fromString(QString key, Qt::CaseSensitivity cs)
{
    QMetaEnum metaEnum = staticMetaObject.enumerator(0);

    if (cs == Qt::CaseInsensitive)
        key = key.toUpper();

    bool ok;
    Enum value = static_cast<Enum>(metaEnum.keyToValue(key.toLatin1().data(), &ok));
    if (!ok)
        return unknown;

    return value;
}

bool DataType::isNumeric(DataType::Enum e)
{
    switch (e)
    {
        case BIGINT:
        case DECIMAL:
        case DOUBLE:
        case INTEGER:
        case INT:
        case NUMERIC:
        case REAL:
            return true;
        case BLOB:
        case BOOLEAN:
        case CHAR:
        case DATE:
        case DATETIME:
        case NONE:
        case STRING:
        case TEXT:
        case TIME:
        case VARCHAR:
        case unknown:
            break;
    }
    return false;
}

bool DataType::isBinary(const QString& type)
{
    static const QStringList binaryTypes = {"BLOB", "CLOB", "LOB"};
    return binaryTypes.contains(type.toUpper());
}

QList<DataType::Enum> DataType::getAllTypes()
{
    return values;
}

QList<DataType::Enum> DataType::getAllTypesForUiDropdown()
{
    return valuesForUiDropdown;
}

QStringList DataType::getAllNames()
{
    return names;
}
